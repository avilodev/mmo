/**
 * @file
 * Drain the friend mutation queue, apply it to the graph, and publish the result.
 *
 * See friends.h for why the realm owns these writes and for the commit-then-
 * cache-then-publish ordering every path below follows.
 */
#include "realm_friends.h"

#include "friend_bus.h"
#include "log.h"
#include "presence.h"
#include "protocol.h"        /* FRIEND_WIRE_* and FRIEND_ACTION_*: the values the client sees */
#include "social_database.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/** Seconds the consumer blocks on the queue before rechecking the stop flag.
 *
 * Also the shutdown latency, and the only cost of an idle bus: one BRPOP that
 * returns nothing every two seconds.
 */
#define CONSUMER_BLOCK_SECONDS 2

static pthread_t  g_consumer_thread;
static atomic_int g_consumer_running = 0;

/* --- Result mapping ------------------------------------------------------ */

/** Translate a database outcome into the value the client is told.
 *
 * FRIEND_RESULT_BLOCKED becomes FRIEND_WIRE_OK, deliberately and in exactly one
 * place. A sender who learns they are blocked makes a second account; a sender
 * whose request appears to have been sent, and is simply never answered, learns
 * nothing at all. The block is still honoured -- nothing was written.
 */
static uint8_t wire_result(FriendResult r) {
    switch (r) {
        case FRIEND_RESULT_OK:              return FRIEND_WIRE_OK;
        case FRIEND_RESULT_BLOCKED:         return FRIEND_WIRE_OK;
        case FRIEND_RESULT_ALREADY_FRIENDS: return FRIEND_WIRE_ALREADY_FRIENDS;
        case FRIEND_RESULT_ALREADY_PENDING: return FRIEND_WIRE_ALREADY_PENDING;
        case FRIEND_RESULT_MUTUAL:          return FRIEND_WIRE_MUTUAL;
        case FRIEND_RESULT_SELF:            return FRIEND_WIRE_SELF;
        case FRIEND_RESULT_NOT_FOUND:       return FRIEND_WIRE_NOT_FOUND;
        case FRIEND_RESULT_FRIEND_CAP:      return FRIEND_WIRE_FRIEND_CAP;
        case FRIEND_RESULT_PENDING_CAP:     return FRIEND_WIRE_PENDING_CAP;
        case FRIEND_RESULT_ERROR:
        default:                            return FRIEND_WIRE_ERROR;
    }
}

/** Name the client-facing action a mutation is reporting on. */
static uint8_t wire_action(FriendOp op) {
    switch (op) {
        case FRIEND_OP_REQUEST: return FRIEND_ACTION_REQUEST;
        case FRIEND_OP_ACCEPT:  return FRIEND_ACTION_ACCEPT;
        case FRIEND_OP_DECLINE: return FRIEND_ACTION_DECLINE;
        case FRIEND_OP_CANCEL:  return FRIEND_ACTION_REMOVE;
        case FRIEND_OP_REMOVE:  return FRIEND_ACTION_REMOVE;
        case FRIEND_OP_BLOCK:   return FRIEND_ACTION_BLOCK;
        case FRIEND_OP_UNBLOCK: return FRIEND_ACTION_UNBLOCK;
        default:                return FRIEND_ACTION_REQUEST;
    }
}

/* --- Publishing ---------------------------------------------------------- */

/** Find the world a player is on, so an event can be addressed to it.
 *
 * @return The world id, or 0 when the account is offline -- in which case
 *         nothing is published and nothing is lost: the durable rows are
 *         already written and their panel reads them on next entry.
 */
static uint32_t world_of(uint32_t account_id) {
    PresenceRecord rec;
    return presence_get(account_id, &rec) ? rec.world_id : 0;
}

/** Tell one player what became of the action they asked for. */
static void publish_result(const FriendMutation* m, uint8_t result) {
    if (!m->origin_world_id) return;

    FriendEvent e = {
        .type         = FRIEND_EVENT_RESULT,
        .account_id   = m->actor_account,
        .character_id = m->actor_character_id,
        .action       = wire_action(m->op),
        .result       = result,
        .peer_account = m->target_account,
    };
    snprintf(e.peer_name, sizeof(e.peer_name), "%s", m->target_name);

    friend_event_publish(m->origin_world_id, &e);
}

void friends_refresh_caches(uint32_t account_id, uint32_t world_id) {
    if (!account_id) return;

    /* Friends first. */
    FriendEntry friends[MAX_FRIENDS];
    int friend_count = social_friend_list(account_id, friends, MAX_FRIENDS);

    if (friend_count >= 0) {
        /* The name travels with the id. A friend who is offline has no presence
         * record for a world server to read a name from, and this is the only
         * place that has both -- the durable rows were just read above. */
        FriendCacheEntry cache[MAX_FRIENDS];
        for (int i = 0; i < friend_count; i++) {
            cache[i].account_id = friends[i].account_id;
            snprintf(cache[i].name, sizeof(cache[i].name), "%s",
                     friends[i].last_character_name);
        }
        presence_friends_store(account_id, cache, friend_count);
    }

    /* Then the pending requests. Read through the database rather than patched
     * incrementally: this also runs the lazy TTL sweep, so opening a panel is
     * what expires stale requests. */
    FriendRequestEntry requests[MAX_FRIEND_REQUESTS_PER_PACKET];
    int request_count = social_friend_requests_incoming(account_id, requests,
                                                        MAX_FRIEND_REQUESTS_PER_PACKET);

    if (request_count >= 0) {
        FriendRequestCache cache[MAX_FRIEND_REQUESTS_PER_PACKET];
        for (int i = 0; i < request_count; i++) {
            cache[i].from_account = requests[i].from_account;
            cache[i].created_at   = requests[i].created_at;
            snprintf(cache[i].from_name, sizeof(cache[i].from_name), "%s",
                     requests[i].from_name);
        }
        presence_requests_store(account_id, cache, request_count);
    }

    /* Then the blocks. Not shown in any panel -- this one is published purely
     * so the worlds can filter chat and invitations without asking the realm
     * per message. Stored even when empty, because "blocks nobody" is the
     * common case and is exactly what a world most needs to learn cheaply. */
    uint32_t blocked[MAX_BLOCKS];
    int block_count = social_block_list(account_id, blocked, MAX_BLOCKS);
    if (block_count >= 0) presence_blocks_store(account_id, blocked, block_count);

    if (!world_id) return;

    FriendEvent e = {
        .type       = FRIEND_EVENT_LIST_READY,
        .account_id = account_id,
    };
    friend_event_publish(world_id, &e);
}

/** Refresh both sides of a relationship that just changed.
 *
 * The peer is refreshed even when offline: only the announcement needs a world,
 * and leaving their cache stale would show them a friend they no longer have
 * the moment they next log in, until something else happened to rebuild it.
 */
static void refresh_pair(uint32_t actor, uint32_t peer, uint32_t actor_world) {
    friends_refresh_caches(actor, actor_world);
    if (peer) friends_refresh_caches(peer, world_of(peer));
}

/* --- The decision table -------------------------------------------------- */

/** Resolve the account a mutation is about.
 *
 * A player types a character name, so most mutations arrive with a name and no
 * id. A world server that already knew the id -- because the target was
 * standing next to them -- sends it and skips this.
 *
 * @return 1 when `out` names an account, 0 when the name resolves to nobody.
 */
static int resolve_target(const FriendMutation* m, uint32_t* out) {
    if (m->target_account) { *out = m->target_account; return 1; }
    if (!m->target_name[0]) { *out = 0; return 0; }

    return social_account_by_character_name(m->target_name, m->origin_world_id, out);
}

void friends_apply(const FriendMutation* m) {
    if (!m) return;

    /* A panel open is not a mutation and has no target: it only asks for the
     * caches to be rebuilt and announced. */
    if (m->op == FRIEND_OP_LIST) {
        friends_refresh_caches(m->actor_account, m->origin_world_id);
        return;
    }

    uint32_t target = 0;
    if (!resolve_target(m, &target)) {
        /* Reported as not-found whether the name is unknown or merely ambiguous.
         * Distinguishing the two would confirm that a name exists somewhere,
         * which is more than the asking player is owed. */
        publish_result(m, FRIEND_WIRE_NOT_FOUND);
        return;
    }

    FriendMutation resolved = *m;
    resolved.target_account = target;

    FriendResult r = FRIEND_RESULT_ERROR;

    switch (m->op) {
        case FRIEND_OP_REQUEST:
            r = social_friend_request(m->actor_account, target);

            if (r == FRIEND_RESULT_OK) {
                /* The target's request list changed, so their cache is stale.
                 * Refreshed before the notify, so a client that reacts to the
                 * notify by opening the panel reads the new state. */
                uint32_t target_world = world_of(target);
                friends_refresh_caches(target, target_world);

                if (target_world) {
                    FriendEvent notify = {
                        .type         = FRIEND_EVENT_REQUEST_NOTIFY,
                        .account_id   = target,
                        .peer_account = m->actor_account,
                    };
                    /* Named by the character the sender is currently playing,
                     * which is the only name the recipient could recognise. */
                    PresenceRecord sender;
                    if (presence_get(m->actor_account, &sender))
                        snprintf(notify.peer_name, sizeof(notify.peer_name), "%s",
                                 sender.character_name);

                    friend_event_publish(target_world, &notify);
                }
            } else if (r == FRIEND_RESULT_MUTUAL) {
                /* Two crossing requests collapsed into a friendship, so both
                 * sides have a new friend and neither has a pending request. */
                refresh_pair(m->actor_account, target, m->origin_world_id);
            }
            break;

        case FRIEND_OP_ACCEPT:
            r = social_friend_accept(m->actor_account, target);
            if (r == FRIEND_RESULT_OK) refresh_pair(m->actor_account, target, m->origin_world_id);
            break;

        case FRIEND_OP_DECLINE:
            r = social_friend_decline(m->actor_account, target);
            /* Only the decliner's own list changes, and the sender is not told.
             * Telling somebody they were declined is how you get a second
             * request, and then a whisper. */
            if (r == FRIEND_RESULT_OK)
                friends_refresh_caches(m->actor_account, m->origin_world_id);
            break;

        case FRIEND_OP_CANCEL:
            r = social_friend_cancel(m->actor_account, target);
            if (r == FRIEND_RESULT_OK) refresh_pair(m->actor_account, target, m->origin_world_id);
            break;

        case FRIEND_OP_REMOVE:
            /* One opcode covers "remove this friend" and "withdraw the request I
             * sent them", because they are the same intent and the client has no
             * reliable way to know which of the two it currently has -- its view
             * may be a second out of date. Try the friendship, fall back to the
             * request. */
            r = social_friend_remove(m->actor_account, target);
            if (r == FRIEND_RESULT_NOT_FOUND)
                r = social_friend_cancel(m->actor_account, target);
            if (r == FRIEND_RESULT_OK) refresh_pair(m->actor_account, target, m->origin_world_id);
            break;

        case FRIEND_OP_BLOCK:
            r = social_block_add(m->actor_account, target);
            /* A block tears down the friendship in both directions, so the
             * blocked account's cache is stale too -- and it must be rebuilt, or
             * their panel keeps showing somebody who has removed them. */
            if (r == FRIEND_RESULT_OK) refresh_pair(m->actor_account, target, m->origin_world_id);
            break;

        case FRIEND_OP_UNBLOCK:
            r = social_block_remove(m->actor_account, target);
            /* Only the actor's own caches change -- unblocking restores no
             * friendship and tells the other side nothing -- but they do have
             * to change. This refreshed nothing at all, which was invisible
             * while a block was a row nobody read; now that the worlds filter
             * chat through the cached set, a stale one means an unblock the
             * player performed goes on silencing somebody until the cache
             * happens to expire. */
            if (r == FRIEND_RESULT_OK)
                friends_refresh_caches(m->actor_account, m->origin_world_id);
            break;

        default:
            LOG_WARN("[FRIENDS] unhandled op %d from account %u",
                     (int)m->op, m->actor_account);
            return;
    }

    if (r == FRIEND_RESULT_BLOCKED) {
        /* Logged here and nowhere the sender can see. Somebody being repeatedly
         * refused by a block is worth knowing about on the server. */
        LOG_INFO("[FRIENDS] account %u is blocked by %u; request swallowed",
                 m->actor_account, target);
    }

    publish_result(&resolved, wire_result(r));
}

/* --- The consumer -------------------------------------------------------- */

/** Drain the queue until asked to stop. */
static void* consumer_thread(void* arg) {
    (void)arg;
    LOG_INFO("[FRIENDS] mutation consumer started");

    while (atomic_load(&g_consumer_running)) {
        FriendMutation m;
        int got = friend_mutation_pop(&m, CONSUMER_BLOCK_SECONDS);

        if (got == 1) {
            friends_apply(&m);
            continue;
        }
        if (got == 0) continue;         /* nothing queued; go back and wait */

        /* Redis is unreachable. Backing off rather than spinning: the queue is
         * durable, so nothing is lost while this waits, and a tight reconnect
         * loop against a Redis that is down is the thing most likely to keep it
         * down. */
        for (int i = 0; i < CONSUMER_BLOCK_SECONDS && atomic_load(&g_consumer_running); i++)
            sleep(1);
    }

    LOG_INFO("[FRIENDS] mutation consumer stopped");
    return NULL;
}

int friends_start(const char* db_path) {
    if (atomic_load(&g_consumer_running)) return 1;    /* idempotent */

    if (!social_db_init(db_path)) {
        LOG_ERROR("[FRIENDS] could not open the social database at %s; "
                  "friends and presence are disabled this run", db_path);
        return 0;
    }

    atomic_store(&g_consumer_running, 1);

    if (pthread_create(&g_consumer_thread, NULL, consumer_thread, NULL) != 0) {
        LOG_ERROR("[FRIENDS] could not start the mutation consumer thread");
        atomic_store(&g_consumer_running, 0);
        social_db_close();
        return 0;
    }
    return 1;
}

void friends_stop(void) {
    if (!atomic_exchange(&g_consumer_running, 0)) return;

    /* Joined before the database closes. A consumer still inside
     * social_friend_accept() when the handle went away would be writing through
     * a freed sqlite3*. */
    pthread_join(g_consumer_thread, NULL);
    presence_consumer_close();
    social_db_close();
}
