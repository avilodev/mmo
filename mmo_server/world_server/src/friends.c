/**
 * @file
 * Serve the friends panel from this world and watch friends on the other nine.
 *
 * See friends.h for the reverse index and the threading rules. The short
 * version: this file owns no data, forwards every change to the realm, and
 * turns what comes back into packets for one local session.
 */
#include "friends.h"

#include "friend_bus.h"
#include "log.h"
#include "player_data.h"
#include "presence.h"
#include "protocol.h"
#include "utils.h"

#include <arpa/inet.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

/** Buckets in the reverse index.
 *
 * A power of two so the modulo is a mask. Sized for MAX_PLAYERS local players
 * holding a full friends list each, which is the worst case this world can
 * reach: 1000 * 100 entries over 8192 buckets is a chain of about twelve.
 */
#define WATCH_BUCKETS 8192

/** Entries the index can hold.
 *
 * The hard ceiling, and it is deliberate: an index that grew on demand would
 * make a player's friends list a lever on this server's memory. Past this the
 * newest watches are simply not registered -- the panel still works, because it
 * reads presence directly when it opens; only the live "came online" push is
 * lost, which is the least important thing here.
 */
#define WATCH_ENTRIES (MAX_PLAYERS * 32)

/** One "this local character is watching that account" edge. */
typedef struct {
    uint32_t watched_account;
    uint32_t watcher_character;
    int      next;              /**< Next entry in this bucket, or -1. */
} WatchEntry;

static WatchEntry      g_watch[WATCH_ENTRIES];
static int             g_buckets[WATCH_BUCKETS];
static int             g_free_head = -1;
static int             g_watch_used = 0;
static pthread_mutex_t g_watch_lock = PTHREAD_MUTEX_INITIALIZER;

static uint32_t   g_world_id = 0;
static atomic_int g_ready    = 0;

/** Mix an account id into a bucket.
 *
 * Account ids are a dense sequence from a SERIAL, so the low bits alone would
 * put every account that logged in together into adjacent buckets. Knuth's
 * multiplicative constant spreads them.
 */
static inline int bucket_of(uint32_t account_id) {
    return (int)((account_id * 2654435761u) & (WATCH_BUCKETS - 1));
}

/* --- The reverse index --------------------------------------------------- */

/** Reset the index to empty. Caller must hold g_watch_lock. */
static void watch_reset(void) {
    for (int i = 0; i < WATCH_BUCKETS; i++) g_buckets[i] = -1;

    for (int i = 0; i < WATCH_ENTRIES - 1; i++) g_watch[i].next = i + 1;
    g_watch[WATCH_ENTRIES - 1].next = -1;

    g_free_head  = 0;
    g_watch_used = 0;
}

/** Register one edge. Caller must hold g_watch_lock. */
static void watch_add(uint32_t watched_account, uint32_t watcher_character) {
    if (g_free_head < 0) {
        /* Rate-limited: this fires once per friend of every player who logs in
         * after the table fills, which is exactly when the log is least useful
         * and most voluminous. */
        LOG_WARN_RL(1, 300, "[FRIENDS] watch index full at %d entries; "
                            "live presence pushes will be missed", WATCH_ENTRIES);
        return;
    }

    int slot = g_free_head;
    g_free_head = g_watch[slot].next;

    int b = bucket_of(watched_account);
    g_watch[slot].watched_account   = watched_account;
    g_watch[slot].watcher_character = watcher_character;
    g_watch[slot].next              = g_buckets[b];
    g_buckets[b] = slot;
    g_watch_used++;
}

/** Remove every edge belonging to one local character.
 *
 * Walks all buckets rather than keeping a per-watcher chain. This runs once per
 * logout and touches 8192 heads plus the entries hanging off them, which is
 * microseconds -- and a second chain through every entry would be a second
 * thing to keep consistent on every insert.
 */
static void watch_remove_watcher(uint32_t watcher_character) {
    for (int b = 0; b < WATCH_BUCKETS; b++) {
        int  index = g_buckets[b];
        int* link  = &g_buckets[b];

        while (index >= 0) {
            int next = g_watch[index].next;

            if (g_watch[index].watcher_character == watcher_character) {
                *link = next;
                g_watch[index].next = g_free_head;
                g_free_head = index;
                g_watch_used--;
            } else {
                link = &g_watch[index].next;
            }
            index = next;
        }
    }
}

/** Collect the local characters watching one account.
 *
 * Copied out under the lock rather than dispatched under it: sending a packet
 * takes a player slot lock, and taking a slot lock while holding the index lock
 * is a lock-order inversion against every other path in this file.
 *
 * @return How many were written.
 */
static int watchers_of(uint32_t watched_account, uint32_t* out, int max) {
    int n = 0;

    pthread_mutex_lock(&g_watch_lock);
    for (int index = g_buckets[bucket_of(watched_account)]; index >= 0 && n < max;
         index = g_watch[index].next) {
        if (g_watch[index].watched_account == watched_account)
            out[n++] = g_watch[index].watcher_character;
    }
    pthread_mutex_unlock(&g_watch_lock);

    return n;
}

int world_friends_watch_count(void) {
    pthread_mutex_lock(&g_watch_lock);
    int n = g_watch_used;
    pthread_mutex_unlock(&g_watch_lock);
    return n;
}

/* --- Sending ------------------------------------------------------------- */

/** Send one packet to a local character, or drop it if they have gone.
 *
 * The descriptor is read under the slot lock and used after releasing it, which
 * is what party.c does for the same reason: server_send() can block, and
 * blocking with a player slot held stalls every other thread that wants it.
 */
static void send_to_character(uint32_t character_id, void* packet, size_t size) {
    ActivePlayer* p = player_acquire(character_id);
    if (!p) return;

    int fd = p->client_fd;
    player_release(p);

    if (fd > 0) server_send(fd, packet, size);
}

/** Find the local character an account is playing, if any.
 *
 * The index answers the other direction, so this is a scan -- but it runs only
 * for an event addressed to this world, which the realm publishes only when
 * presence already said the account is here.
 *
 * @return The character id, or 0 when the account is not on this world.
 */
static uint32_t local_character_of(uint32_t account_id) {
    for (int slot = 0; slot < MAX_PLAYERS; slot++) {
        ActivePlayer* p = player_acquire_slot(slot, 0);
        if (!p) continue;

        uint32_t character = (p->account_id == account_id) ? p->character_id : 0;
        player_release(p);

        if (character) return character;
    }
    return 0;
}

/** Fill and send the friends list from the Redis caches.
 *
 * Two Redis reads: the cached set of friend account ids, then one MGET across
 * their presence keys. No PostgreSQL, no realm, no fan-out across worlds --
 * which is the entire point of the caches.
 */
static void send_friend_list(uint32_t character_id, uint32_t account_id) {
    FriendCacheEntry cached[MAX_FRIENDS_PER_PACKET];
    int count = presence_friends_load(account_id, cached, MAX_FRIENDS_PER_PACKET);

    if (count < 0) {
        /* Cache miss. Ask the realm to rebuild it; a LIST_READY event brings us
         * back here. Nothing is sent now, because sending an empty list would
         * render as "you have no friends" rather than as "still loading". */
        FriendMutation m = {
            .op                 = FRIEND_OP_LIST,
            .actor_account      = account_id,
            .origin_world_id    = g_world_id,
            .actor_character_id = character_id,
        };
        friend_mutation_push(&m);
        return;
    }

    static _Thread_local FriendListResponsePacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.header.type = PACKET_FRIEND_LIST_RESPONSE;

    uint32_t ids[MAX_FRIENDS_PER_PACKET];
    for (int i = 0; i < count; i++) ids[i] = cached[i].account_id;

    PresenceRecord presence[MAX_FRIENDS_PER_PACKET];
    if (count > 0) presence_get_many(ids, count, presence);

    for (int i = 0; i < count; i++) {
        FriendWireEntry* e = &pkt.friends[i];
        e->account_id = htonl(cached[i].account_id);

        /* The cached name is the fallback and the live one wins. They differ
         * only while a friend is playing a character they were not last seen
         * on, and in that window the live one is what the player is looking at
         * across the world. */
        snprintf(e->name, sizeof(e->name), "%s", cached[i].name);

        if (presence[i].online) {
            e->online   = 1;
            e->world_id = htonl(presence[i].world_id);
            if (presence[i].character_name[0])
                snprintf(e->name, sizeof(e->name), "%s", presence[i].character_name);
        }
        /* Offline leaves world_id and online at zero. There is nothing else to
         * say: the game keeps no record of where somebody used to be, so the
         * panel shows the name and the word Offline and stops there. */
    }

    pkt.count = (uint8_t)count;

    /* Only the entries actually used are sent, so a player with three friends
     * costs 163 bytes rather than five kilobytes of zeroes. */
    size_t size = offsetof(FriendListResponsePacket, friends)
                + (size_t)count * sizeof(FriendWireEntry);
    pkt.header.payload_size = htons((uint16_t)(size - sizeof(PacketHeader)));

    send_to_character(character_id, &pkt, size);
}

/** Fill and send the pending requests from the Redis cache. */
static void send_request_list(uint32_t character_id, uint32_t account_id) {
    FriendRequestCache cache[MAX_FRIEND_REQUESTS_PER_PACKET];
    int count = presence_requests_load(account_id, cache, MAX_FRIEND_REQUESTS_PER_PACKET);

    /* A miss here is not worth a round trip of its own: send_friend_list() has
     * already asked the realm to rebuild both caches, and the LIST_READY that
     * answers it brings us back. */
    if (count < 0) return;

    static _Thread_local FriendRequestsListPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.header.type = PACKET_FRIEND_REQUESTS_LIST;

    for (int i = 0; i < count; i++) {
        pkt.requests[i].from_account = htonl(cache[i].from_account);
        pkt.requests[i].created_at   = (int64_t)mmo_htonll((uint64_t)cache[i].created_at);
        snprintf(pkt.requests[i].from_name, sizeof(pkt.requests[i].from_name), "%s",
                 cache[i].from_name);
    }
    pkt.count = (uint8_t)count;

    size_t size = offsetof(FriendRequestsListPacket, requests)
                + (size_t)count * sizeof(FriendRequestWireEntry);
    pkt.header.payload_size = htons((uint16_t)(size - sizeof(PacketHeader)));

    send_to_character(character_id, &pkt, size);
}

/* --- Events from the realm ----------------------------------------------- */

/** Rebuild one local player's watch set from their cached friend list. */
static void rebuild_watch_set(uint32_t account_id, uint32_t character_id) {
    FriendCacheEntry cached[MAX_FRIENDS_PER_PACKET];
    int count = presence_friends_load(account_id, cached, MAX_FRIENDS_PER_PACKET);
    if (count < 0) return;      /* uncached; the next LIST_READY rebuilds it */

    pthread_mutex_lock(&g_watch_lock);
    watch_remove_watcher(character_id);
    for (int i = 0; i < count; i++) watch_add(cached[i].account_id, character_id);
    pthread_mutex_unlock(&g_watch_lock);
}

/** Handle one event addressed to this world. Runs on the bus thread. */
static void on_friend_event(const FriendEvent* e, void* user) {
    (void)user;

    /* An event names an account; the session it belongs to is whichever
     * character of theirs is on this world. character_id is a hint the realm
     * echoes back from the mutation, and it is only trusted after confirming
     * the account still matches -- the player may have swapped characters
     * between asking and being answered. */
    uint32_t character = e->character_id;
    if (character) {
        ActivePlayer* p = player_acquire(character);
        int matches = p && p->account_id == e->account_id;
        if (p) player_release(p);
        if (!matches) character = 0;
    }
    if (!character) character = local_character_of(e->account_id);
    if (!character) return;              /* they left; nothing to deliver to */

    switch (e->type) {
        case FRIEND_EVENT_RESULT: {
            FriendOpResultPacket pkt = {0};
            pkt.header.type         = PACKET_FRIEND_OP_RESULT;
            pkt.header.payload_size = htons((uint16_t)(sizeof(pkt) - sizeof(PacketHeader)));
            pkt.action = e->action;
            pkt.result = e->result;
            snprintf(pkt.subject_name, sizeof(pkt.subject_name), "%s", e->peer_name);

            send_to_character(character, &pkt, sizeof(pkt));
            break;
        }

        case FRIEND_EVENT_LIST_READY:
            /* The caches are fresh. Re-read them, push the panel, and re-invert
             * the friend set: a friendship that just appeared or vanished is
             * exactly a watch that has to start or stop. */
            rebuild_watch_set(e->account_id, character);
            send_friend_list(character, e->account_id);
            send_request_list(character, e->account_id);
            break;

        case FRIEND_EVENT_REQUEST_NOTIFY: {
            FriendRequestNotifyPacket pkt = {0};
            pkt.header.type         = PACKET_FRIEND_REQUEST_NOTIFY;
            pkt.header.payload_size = htons((uint16_t)(sizeof(pkt) - sizeof(PacketHeader)));
            pkt.from_account = htonl(e->peer_account);
            snprintf(pkt.from_name, sizeof(pkt.from_name), "%s", e->peer_name);

            send_to_character(character, &pkt, sizeof(pkt));

            /* The request list changed, so push it too. The notify alone is a
             * popup; this is what makes the panel agree with it. */
            send_request_list(character, e->account_id);
            break;
        }

        default:
            break;
    }
}

/** Handle one presence change from anywhere. Runs on the bus thread.
 *
 * Every world hears every change, so the first thing this does is decide
 * whether anybody here cares -- one hash lookup, and for almost every event the
 * answer is no.
 */
static void on_presence_change(const PresenceRecord* rec, void* user) {
    (void)user;

    uint32_t watchers[MAX_PLAYERS];
    int n = watchers_of(rec->account_id, watchers, MAX_PLAYERS);
    if (n == 0) return;

    FriendPresenceUpdatePacket pkt = {0};
    pkt.header.type         = PACKET_FRIEND_PRESENCE_UPDATE;
    pkt.header.payload_size = htons((uint16_t)(sizeof(pkt) - sizeof(PacketHeader)));
    pkt.account_id = htonl(rec->account_id);
    pkt.online     = rec->online ? 1 : 0;

    if (rec->online) {
        pkt.world_id = htonl(rec->world_id);
        snprintf(pkt.name, sizeof(pkt.name), "%s", rec->character_name);
    }

    for (int i = 0; i < n; i++) send_to_character(watchers[i], &pkt, sizeof(pkt));
}

/* --- Client packets ------------------------------------------------------ */

/** Queue one mutation for the realm, naming who asked and from where. */
static void forward(FriendOp op, uint32_t account_id, uint32_t character_id,
                    const char* target_name) {
    FriendMutation m = {
        .op                 = op,
        .actor_account      = account_id,
        .origin_world_id    = g_world_id,
        .actor_character_id = character_id,
    };
    if (target_name) snprintf(m.target_name, sizeof(m.target_name), "%s", target_name);

    if (!friend_mutation_push(&m)) {
        /* Redis refused the queue. The player is told rather than left watching
         * a button that did nothing. */
        FriendOpResultPacket pkt = {0};
        pkt.header.type         = PACKET_FRIEND_OP_RESULT;
        pkt.header.payload_size = htons((uint16_t)(sizeof(pkt) - sizeof(PacketHeader)));
        pkt.result = FRIEND_WIRE_UNAVAILABLE;
        if (target_name) snprintf(pkt.subject_name, sizeof(pkt.subject_name), "%s", target_name);

        send_to_character(character_id, &pkt, sizeof(pkt));
    }
}

/** Copy a wire name field into a terminated buffer.
 *
 * Nothing off the wire is terminated. Every handler below reads its name
 * through this rather than trusting the packet, because the very next thing
 * that happens to it is an snprintf("%s").
 */
static void copy_name(char* out, size_t out_size, const char* raw, size_t raw_size) {
    size_t n = raw_size < out_size - 1 ? raw_size : out_size - 1;
    memcpy(out, raw, n);
    out[n] = '\0';
}

void world_friends_handle_packet(int client_fd, uint32_t character_id,
                                 const uint8_t* buffer, ssize_t bytes) {
    (void)client_fd;
    if (bytes < (ssize_t)sizeof(PacketHeader)) return;

    /* Read from the slot, never from the packet: the account is what the realm
     * acts on, and a client that could name its own would be able to unfriend
     * on somebody else's behalf. */
    ActivePlayer* p = player_acquire(character_id);
    uint32_t account_id = p ? p->account_id : 0;
    if (p) player_release(p);
    if (!account_id) return;

    const PacketHeader* header = (const PacketHeader*)buffer;
    char name[32];

    switch (header->type) {
        case PACKET_FRIEND_REQUEST: {
            if (bytes < (ssize_t)sizeof(FriendRequestPacket)) return;
            const FriendRequestPacket* req = (const FriendRequestPacket*)buffer;
            copy_name(name, sizeof(name), req->target_name, sizeof(req->target_name));
            if (!name[0]) return;
            forward(FRIEND_OP_REQUEST, account_id, character_id, name);
            break;
        }

        case PACKET_FRIEND_RESPOND: {
            if (bytes < (ssize_t)sizeof(FriendRespondPacket)) return;
            const FriendRespondPacket* req = (const FriendRespondPacket*)buffer;
            copy_name(name, sizeof(name), req->from_name, sizeof(req->from_name));
            if (!name[0]) return;
            forward(req->accept ? FRIEND_OP_ACCEPT : FRIEND_OP_DECLINE,
                    account_id, character_id, name);
            break;
        }

        case PACKET_FRIEND_REMOVE: {
            if (bytes < (ssize_t)sizeof(FriendRemovePacket)) return;
            const FriendRemovePacket* req = (const FriendRemovePacket*)buffer;
            copy_name(name, sizeof(name), req->target_name, sizeof(req->target_name));
            if (!name[0]) return;
            forward(FRIEND_OP_REMOVE, account_id, character_id, name);
            break;
        }

        case PACKET_FRIEND_BLOCK: {
            if (bytes < (ssize_t)sizeof(FriendBlockPacket)) return;
            const FriendBlockPacket* req = (const FriendBlockPacket*)buffer;
            copy_name(name, sizeof(name), req->target_name, sizeof(req->target_name));
            if (!name[0]) return;
            forward(req->block ? FRIEND_OP_BLOCK : FRIEND_OP_UNBLOCK,
                    account_id, character_id, name);
            break;
        }

        case PACKET_FRIEND_LIST_REQUEST:
            /* Answered from the caches when they are warm, which is the common
             * case and costs the realm nothing. A miss falls through to a
             * mutation and comes back as LIST_READY. */
            send_friend_list(character_id, account_id);
            send_request_list(character_id, account_id);
            break;

        default:
            break;
    }
}

/* --- Lifecycle ----------------------------------------------------------- */

int world_friends_init(uint32_t world_id) {
    g_world_id = world_id;

    pthread_mutex_lock(&g_watch_lock);
    watch_reset();
    pthread_mutex_unlock(&g_watch_lock);

    if (!friend_bus_start(world_id, on_friend_event, on_presence_change, NULL)) {
        LOG_WARN("[FRIENDS] the event bus did not start; friends will not update live");
        return 0;
    }

    atomic_store(&g_ready, 1);
    LOG_INFO("[FRIENDS] ready on world %u", world_id);
    return 1;
}

void world_friends_shutdown(void) {
    if (!atomic_exchange(&g_ready, 0)) return;

    /* The bus is stopped and joined first. Its callbacks reach into the player
     * pool and into this file's index, and one still running while the world
     * tears down is a shutdown crash that only ever happens on the live host. */
    friend_bus_stop();
    presence_consumer_close();

    pthread_mutex_lock(&g_watch_lock);
    watch_reset();
    pthread_mutex_unlock(&g_watch_lock);
}

void world_friends_player_entered(uint32_t account_id, uint32_t character_id,
                                  const char* character_name) {
    if (!atomic_load(&g_ready) || !account_id) return;

    /* Presence first: it is what makes this player findable by name, resolvable
     * by the realm, and visible to everybody already watching them. */
    presence_set(account_id, g_world_id, character_id, character_name);

    /* Then their own view. A cache miss here asks the realm to build it, and
     * the LIST_READY that answers does the inversion instead. */
    rebuild_watch_set(account_id, character_id);
    send_friend_list(character_id, account_id);
    send_request_list(character_id, account_id);
}

void world_friends_player_left(uint32_t account_id, uint32_t character_id) {
    if (!atomic_load(&g_ready)) return;

    pthread_mutex_lock(&g_watch_lock);
    watch_remove_watcher(character_id);
    pthread_mutex_unlock(&g_watch_lock);

    /* Guarded rather than unconditional: on a world transfer this leave can
     * land after the new world has already written its own presence, and an
     * unconditional DEL there would strand the player as offline for the rest
     * of their session. See presence_clear_if_character(). */
    if (account_id) presence_clear_if_character(account_id, character_id);
}

void world_friends_heartbeat(void) {
    if (!atomic_load(&g_ready)) return;

    /* Collected first, refreshed second. The refresh is a Redis round trip per
     * player and holding a slot lock across it would stall the tick. */
    static uint32_t accounts[MAX_PLAYERS];
    int n = 0;

    for (int slot = 0; slot < MAX_PLAYERS && n < MAX_PLAYERS; slot++) {
        ActivePlayer* p = player_acquire_slot(slot, 0);
        if (!p) continue;

        if (p->account_id) accounts[n++] = p->account_id;
        player_release(p);
    }

    if (n) presence_refresh_many(accounts, n);
}
