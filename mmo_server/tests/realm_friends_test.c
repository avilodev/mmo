/**
 * @file
 * Check the realm's mutation handling end to end: queue in, graph changed,
 * caches refreshed, event out.
 *
 * This is the integration seam the unit suites cannot cover. social_database
 * knows the graph but not the caches; presence knows the caches but not the
 * graph; friend_bus knows the wire but neither. What breaks in production is
 * the joins between them -- a mutation applied but never announced, an
 * announcement addressed to the wrong world, a cache left stale on the side
 * that did not ask for the change.
 *
 * Needs Redis. Skips itself without one.
 */

#include "realm_friends.h"
#include "friend_bus.h"
#include "presence.h"
#include "session.h"
#include "social_database.h"
#include "log.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int g_failures = 0;

#define CHECK(cond, what)                                                   \
    do {                                                                    \
        if (cond) {                                                         \
            printf("  ok   %s\n", (what));                                  \
        } else {                                                            \
            printf("  FAIL %s  (%s:%d)\n", (what), __FILE__, __LINE__);     \
            g_failures++;                                                   \
        }                                                                   \
    } while (0)

/* Two accounts on one world, and a third somewhere else. */
#define ALICE      770001u
#define BOB        770002u
#define CAROL      770003u
#define TEST_WORLD 6u

static char g_db_path[512];

/** Everything the subscriber has seen, under a lock the waits use. */
typedef struct {
    pthread_mutex_t lock;
    int             results;
    int             notifies;
    int             lists;
    FriendEvent     last_result;
    FriendEvent     last_notify;
} Seen;

static Seen g_seen;

static void on_event(const FriendEvent* e, void* user) {
    (void)user;
    pthread_mutex_lock(&g_seen.lock);
    switch (e->type) {
        case FRIEND_EVENT_RESULT:         g_seen.last_result = *e; g_seen.results++;  break;
        case FRIEND_EVENT_REQUEST_NOTIFY: g_seen.last_notify = *e; g_seen.notifies++; break;
        case FRIEND_EVENT_LIST_READY:     g_seen.lists++; break;
        default: break;
    }
    pthread_mutex_unlock(&g_seen.lock);
}

static void on_presence(const PresenceRecord* rec, void* user) { (void)rec; (void)user; }

static int wait_for(int* counter, int target, int ms) {
    for (int waited = 0; waited < ms; waited += 10) {
        pthread_mutex_lock(&g_seen.lock);
        int now = *counter;
        pthread_mutex_unlock(&g_seen.lock);
        if (now >= target) return 1;
        usleep(10000);
    }
    return 0;
}

/** Forget every account this suite touches, in Redis and in SQLite. */
static void reset_world(void) {
    const uint32_t all[3] = { ALICE, BOB, CAROL };
    for (int i = 0; i < 3; i++) {
        presence_clear(all[i]);
        presence_friends_drop(all[i]);
        presence_requests_drop(all[i]);
    }

    social_db_close();
    unlink(g_db_path);
    if (!social_db_init(g_db_path)) {
        printf("  FATAL could not open %s\n", g_db_path);
        exit(1);
    }

    /* Alice and Bob are playing on the same world; Carol has played before but
     * is offline now. Presence is what makes a typed name resolvable. */
    social_presence_update(ALICE, TEST_WORLD, 1, "Alice");
    social_presence_update(BOB,   TEST_WORLD, 2, "Bob");
    social_presence_update(CAROL, 9,          3, "Carol");

    presence_set(ALICE, TEST_WORLD, 1, "Alice");
    presence_set(BOB,   TEST_WORLD, 2, "Bob");

    pthread_mutex_lock(&g_seen.lock);
    g_seen.results = g_seen.notifies = g_seen.lists = 0;
    pthread_mutex_unlock(&g_seen.lock);
}

/** Build a mutation the way a world server would. */
static FriendMutation by_name(FriendOp op, uint32_t actor, const char* name) {
    FriendMutation m = {
        .op                 = op,
        .actor_account      = actor,
        .origin_world_id    = TEST_WORLD,
        .actor_character_id = 1,
    };
    snprintf(m.target_name, sizeof(m.target_name), "%s", name ? name : "");
    return m;
}

/* --- Cases --------------------------------------------------------------- */

static void test_a_request_by_name_reaches_its_target(void) {
    printf("realm: a request typed as a name is applied and announced\n");
    reset_world();

    FriendMutation m = by_name(FRIEND_OP_REQUEST, ALICE, "Bob");
    friends_apply(&m);

    CHECK(wait_for(&g_seen.results, 1, 2000), "the sender is told what happened");
    pthread_mutex_lock(&g_seen.lock);
    FriendEvent result = g_seen.last_result;
    pthread_mutex_unlock(&g_seen.lock);

    CHECK(result.account_id == ALICE, "the result is addressed to the sender");
    CHECK(result.result == FRIEND_WIRE_OK, "and reports success");

    CHECK(wait_for(&g_seen.notifies, 1, 2000), "the target is notified");
    pthread_mutex_lock(&g_seen.lock);
    FriendEvent notify = g_seen.last_notify;
    pthread_mutex_unlock(&g_seen.lock);

    CHECK(notify.account_id == BOB, "the notify is addressed to the target");
    CHECK(notify.peer_account == ALICE, "and names the sender");
    CHECK(strcmp(notify.peer_name, "Alice") == 0,
          "by the character they are currently playing");

    /* And the target's cache is refreshed, so opening the panel needs no realm. */
    FriendRequestCache pending[4];
    int n = presence_requests_load(BOB, pending, 4);
    CHECK(n == 1, "the target's request cache holds it");
    CHECK(n == 1 && pending[0].from_account == ALICE, "naming the sender");
}

static void test_an_unknown_name_is_reported_not_found(void) {
    printf("realm: a name nobody has is reported, not silently dropped\n");
    reset_world();

    FriendMutation m = by_name(FRIEND_OP_REQUEST, ALICE, "Nobody");
    friends_apply(&m);

    CHECK(wait_for(&g_seen.results, 1, 2000), "the sender is told");
    pthread_mutex_lock(&g_seen.lock);
    FriendEvent result = g_seen.last_result;
    pthread_mutex_unlock(&g_seen.lock);

    CHECK(result.result == FRIEND_WIRE_NOT_FOUND, "that there is no such player");
}

static void test_an_accept_caches_both_sides(void) {
    printf("realm: an accept refreshes both players' caches\n");
    reset_world();

    FriendMutation request = by_name(FRIEND_OP_REQUEST, ALICE, "Bob");
    friends_apply(&request);

    FriendMutation accept = by_name(FRIEND_OP_ACCEPT, BOB, "Alice");
    friends_apply(&accept);

    CHECK(social_is_friend(ALICE, BOB) == 1, "the graph has the friendship");
    CHECK(social_is_friend(BOB, ALICE) == 1, "in both directions");

    FriendCacheEntry ids[8];
    CHECK(presence_friends_load(ALICE, ids, 8) == 1 && ids[0].account_id == BOB,
          "the sender's cache holds their new friend");
    /* The name travels with the id. Without it a world server has nothing to
     * label the row with once that friend logs out -- presence is the only
     * other source, and an offline account has none. */
    CHECK(strcmp(ids[0].name, "Bob") == 0, "under the name to display them by");

    CHECK(presence_friends_load(BOB, ids, 8) == 1 && ids[0].account_id == ALICE,
          "and so does the accepter's");
    CHECK(strcmp(ids[0].name, "Alice") == 0, "likewise named");

    FriendRequestCache pending[4];
    CHECK(presence_requests_load(BOB, pending, 4) == 0,
          "the accepted request is gone from the cache");
}

static void test_a_block_is_swallowed_not_reported(void) {
    printf("realm: a request to somebody who blocked you looks like success\n");
    reset_world();

    FriendMutation block = by_name(FRIEND_OP_BLOCK, BOB, "Alice");
    friends_apply(&block);

    pthread_mutex_lock(&g_seen.lock);
    g_seen.results = 0;
    pthread_mutex_unlock(&g_seen.lock);

    FriendMutation request = by_name(FRIEND_OP_REQUEST, ALICE, "Bob");
    friends_apply(&request);

    CHECK(wait_for(&g_seen.results, 1, 2000), "the sender is answered");
    pthread_mutex_lock(&g_seen.lock);
    FriendEvent result = g_seen.last_result;
    pthread_mutex_unlock(&g_seen.lock);

    /* The whole point: indistinguishable from a request that landed. */
    CHECK(result.result == FRIEND_WIRE_OK, "and told it succeeded");

    FriendRequestCache pending[4];
    int n = presence_requests_load(BOB, pending, 4);
    CHECK(n <= 0, "while the target never sees a request");
}

static void test_removing_a_friend_clears_both_caches(void) {
    printf("realm: removing a friend clears the other side's cache too\n");
    reset_world();

    FriendMutation request = by_name(FRIEND_OP_REQUEST, ALICE, "Bob");
    friends_apply(&request);
    FriendMutation accept = by_name(FRIEND_OP_ACCEPT, BOB, "Alice");
    friends_apply(&accept);

    FriendMutation remove = by_name(FRIEND_OP_REMOVE, ALICE, "Bob");
    friends_apply(&remove);

    CHECK(social_is_friend(ALICE, BOB) == 0, "the graph dropped it");
    CHECK(social_is_friend(BOB, ALICE) == 0, "in both directions");

    FriendCacheEntry ids[8];
    CHECK(presence_friends_load(ALICE, ids, 8) == 0, "the remover's cache is empty");
    CHECK(presence_friends_load(BOB, ids, 8) == 0,
          "and so is the cache of the person who did not ask");
}

static void test_remove_also_withdraws_a_pending_request(void) {
    printf("realm: remove withdraws a request when there is no friendship\n");
    reset_world();

    FriendMutation request = by_name(FRIEND_OP_REQUEST, ALICE, "Bob");
    friends_apply(&request);

    FriendMutation remove = by_name(FRIEND_OP_REMOVE, ALICE, "Bob");
    friends_apply(&remove);

    FriendRequestCache pending[4];
    CHECK(presence_requests_load(BOB, pending, 4) == 0,
          "the withdrawn request is gone from the target's cache");

    uint32_t out[4];
    CHECK(social_friend_requests_outgoing(ALICE, out, 4) == 0,
          "and out of the sender's outgoing list");
}

static void test_a_panel_open_rebuilds_the_caches(void) {
    printf("realm: opening the panel rebuilds a dropped cache\n");
    reset_world();

    FriendMutation request = by_name(FRIEND_OP_REQUEST, ALICE, "Bob");
    friends_apply(&request);
    FriendMutation accept = by_name(FRIEND_OP_ACCEPT, BOB, "Alice");
    friends_apply(&accept);

    /* A cache can go missing: Redis restarts, a TTL lapses, a write is lost.
     * The panel open is the read-through that heals it. */
    presence_friends_drop(ALICE);
    FriendCacheEntry ids[8];
    CHECK(presence_friends_load(ALICE, ids, 8) == -1, "the cache is gone");

    pthread_mutex_lock(&g_seen.lock);
    g_seen.lists = 0;
    pthread_mutex_unlock(&g_seen.lock);

    FriendMutation list = by_name(FRIEND_OP_LIST, ALICE, "");
    friends_apply(&list);

    CHECK(presence_friends_load(ALICE, ids, 8) == 1 && ids[0].account_id == BOB,
          "and is rebuilt from the durable rows");
    CHECK(wait_for(&g_seen.lists, 1, 2000), "and the world is told to re-read it");
}

static void test_an_offline_peer_still_has_its_cache_refreshed(void) {
    printf("realm: an offline friend's cache is not left stale\n");
    reset_world();

    /* Carol is offline, so nothing can be published to her. Her durable rows
     * and her cache must still be right for when she logs in. */
    FriendMutation request = by_name(FRIEND_OP_REQUEST, CAROL, "Alice");
    friends_apply(&request);
    FriendMutation accept = by_name(FRIEND_OP_ACCEPT, ALICE, "Carol");
    friends_apply(&accept);

    FriendCacheEntry ids[8];
    CHECK(presence_friends_load(CAROL, ids, 8) == 1 && ids[0].account_id == ALICE,
          "the offline account's cache holds the new friendship");
    CHECK(presence_friends_load(ALICE, ids, 8) == 1 && ids[0].account_id == CAROL,
          "and so does the online one's");
}

static void test_crossing_requests_report_mutual(void) {
    printf("realm: two crossing requests are reported as an accept\n");
    reset_world();

    FriendMutation a = by_name(FRIEND_OP_REQUEST, ALICE, "Bob");
    friends_apply(&a);

    /* Wait for the first request's own result to land BEFORE clearing the
     * counter. Events are published asynchronously, so resetting immediately
     * would let that result arrive afterwards and be mistaken for the second
     * request's -- the counter would reach 1 with the wrong event in hand. */
    CHECK(wait_for(&g_seen.results, 1, 2000), "the first request is answered");

    pthread_mutex_lock(&g_seen.lock);
    g_seen.results = 0;
    pthread_mutex_unlock(&g_seen.lock);

    FriendMutation b = by_name(FRIEND_OP_REQUEST, BOB, "Alice");
    friends_apply(&b);

    CHECK(wait_for(&g_seen.results, 1, 2000), "the second sender is answered");
    pthread_mutex_lock(&g_seen.lock);
    FriendEvent result = g_seen.last_result;
    pthread_mutex_unlock(&g_seen.lock);

    CHECK(result.result == FRIEND_WIRE_MUTUAL, "and told they are now friends");
    CHECK(social_is_friend(ALICE, BOB) == 1, "which they are");

    FriendCacheEntry ids[8];
    CHECK(presence_friends_load(ALICE, ids, 8) == 1, "both caches were refreshed");
    CHECK(presence_friends_load(BOB, ids, 8) == 1, "on both sides");
}

static void test_the_consumer_drains_a_queued_mutation(void) {
    printf("realm: a mutation left on the queue is applied by the consumer\n");
    reset_world();

    /* The path a real world server uses: push and walk away. */
    FriendMutation m = by_name(FRIEND_OP_REQUEST, ALICE, "Bob");
    CHECK(friend_mutation_push(&m), "a world server queues a request");

    CHECK(friends_start(g_db_path), "the consumer is running");
    CHECK(wait_for(&g_seen.notifies, 1, 5000), "and the target is notified");

    friends_stop();

    /* friends_stop() closed the database the rest of the suite shares. */
    CHECK(social_db_init(g_db_path), "the database reopens after shutdown");
    CHECK(social_is_friend(ALICE, BOB) == 0, "a request alone is not a friendship");

    FriendRequestCache pending[4];
    CHECK(presence_requests_load(BOB, pending, 4) == 1, "but the request landed");
}

int main(void) {
    log_init();
    pthread_mutex_init(&g_seen.lock, NULL);

    snprintf(g_db_path, sizeof(g_db_path), "/tmp/realm_friends_test_%d.sqlite", (int)getpid());

    if (!session_init() || !session_is_ready()) {
        printf("  SKIP no reachable Redis; the realm suite needs one\n");
        return 0;
    }

    /* Subscribed as if this process were the world these players are on, so the
     * events the realm publishes are actually observed rather than assumed. */
    if (!friend_bus_start(TEST_WORLD, on_event, on_presence, NULL)) {
        printf("  FATAL could not start the event bus\n");
        return 1;
    }
    for (int i = 0; i < 300 && !friend_bus_connected(); i++) usleep(10000);
    if (!friend_bus_connected()) {
        printf("  FATAL the event bus never connected\n");
        return 1;
    }

    test_a_request_by_name_reaches_its_target();
    test_an_unknown_name_is_reported_not_found();
    test_an_accept_caches_both_sides();
    test_a_block_is_swallowed_not_reported();
    test_removing_a_friend_clears_both_caches();
    test_remove_also_withdraws_a_pending_request();
    test_a_panel_open_rebuilds_the_caches();
    test_an_offline_peer_still_has_its_cache_refreshed();
    test_crossing_requests_report_mutual();
    test_the_consumer_drains_a_queued_mutation();

    presence_clear(ALICE);
    presence_clear(BOB);
    friend_bus_stop();
    presence_consumer_close();
    social_db_close();
    session_close();
    unlink(g_db_path);

    if (g_failures) {
        printf("\n%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("\nAll realm friend checks passed\n");
    return 0;
}
