/**
 * @file
 * Check the realm-to-world event bus: its encoding, and that a subscriber
 * actually receives what a publisher sends.
 *
 * The delivery half needs a reachable Redis and skips itself without one. What
 * it is really checking is the part that is easy to get wrong and impossible to
 * notice: a subscriber that connects, reports success, and then silently
 * receives nothing looks exactly like a quiet server.
 */

#include "friend_bus.h"
#include "presence.h"
#include "session.h"
#include "log.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
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

/* --- Encoding ------------------------------------------------------------ */

static void test_event_survives_a_round_trip(void) {
    printf("encode: an event round-trips\n");

    FriendEvent in = {
        .type         = FRIEND_EVENT_RESULT,
        .account_id   = 77,
        .character_id = 501,
        .action       = 3,
        .result       = 2,
        .peer_account = 88,
    };
    snprintf(in.peer_name, sizeof(in.peer_name), "%s", "Thornwood");

    char buf[256];
    CHECK(friend_event_encode(&in, buf, sizeof(buf)) > 0, "it encodes");

    FriendEvent out;
    CHECK(friend_event_decode(buf, &out), "it decodes");
    CHECK(out.type == FRIEND_EVENT_RESULT, "the type survives");
    CHECK(out.account_id == 77, "the account survives");
    CHECK(out.character_id == 501, "the character survives");
    CHECK(out.action == 3, "the action survives");
    CHECK(out.result == 2, "the result survives");
    CHECK(out.peer_account == 88, "the peer survives");
    CHECK(strcmp(out.peer_name, "Thornwood") == 0, "the peer name survives");
}

static void test_event_decode_refuses_rubbish(void) {
    printf("encode: nonsense is refused rather than half-decoded\n");

    FriendEvent out;
    CHECK(!friend_event_decode("", &out), "an empty string is refused");
    CHECK(!friend_event_decode("0|1|2", &out), "a truncated event is refused");
    CHECK(!friend_event_decode("99|1|2|3|4|5|x", &out), "an unknown type is refused");
}

static void test_a_peer_name_cannot_forge_fields(void) {
    printf("encode: a separator in a peer name does not split the message\n");

    FriendEvent in = { .type = FRIEND_EVENT_REQUEST_NOTIFY, .account_id = 5, .peer_account = 6 };
    snprintf(in.peer_name, sizeof(in.peer_name), "%s", "Bad|0|0|0|0|0|z");

    char buf[256];
    CHECK(friend_event_encode(&in, buf, sizeof(buf)) > 0, "it encodes");

    FriendEvent out;
    CHECK(friend_event_decode(buf, &out), "it decodes");
    CHECK(out.account_id == 5, "the account is untouched");
    CHECK(out.peer_account == 6, "the peer is untouched");
    CHECK(strcmp(out.peer_name, "Bad|0|0|0|0|0|z") == 0, "and the name comes back whole");
}

/* --- Delivery ------------------------------------------------------------ */

/** What the subscriber callbacks record, under a lock the test waits on. */
typedef struct {
    pthread_mutex_t lock;
    int             events;
    int             presences;
    FriendEvent     last_event;
    PresenceRecord  last_presence;
} Received;

static Received g_rx;

static void on_event(const FriendEvent* e, void* user) {
    (void)user;
    pthread_mutex_lock(&g_rx.lock);
    g_rx.last_event = *e;
    g_rx.events++;
    pthread_mutex_unlock(&g_rx.lock);
}

static void on_presence(const PresenceRecord* rec, void* user) {
    (void)user;
    pthread_mutex_lock(&g_rx.lock);
    g_rx.last_presence = *rec;
    g_rx.presences++;
    pthread_mutex_unlock(&g_rx.lock);
}

/** Wait up to `ms` for the subscriber to report itself connected.
 *
 * Pub/sub delivers only to subscribers that are already listening, so a test
 * that publishes before the thread has finished its SUBSCRIBE is testing
 * nothing and fails intermittently. This is not politeness, it is the
 * precondition.
 */
static int wait_connected(int ms) {
    for (int waited = 0; waited < ms; waited += 10) {
        if (friend_bus_connected()) return 1;
        usleep(10000);
    }
    return 0;
}

/** Wait up to `ms` for the counter behind `what` to reach `target`. */
static int wait_for(int* counter, int target, int ms) {
    for (int waited = 0; waited < ms; waited += 10) {
        pthread_mutex_lock(&g_rx.lock);
        int now = *counter;
        pthread_mutex_unlock(&g_rx.lock);
        if (now >= target) return 1;
        usleep(10000);
    }
    return 0;
}

static void test_a_published_event_reaches_its_world(void) {
    printf("delivery: an event published for a world reaches that world\n");

    const uint32_t world = 4;
    CHECK(friend_bus_start(world, on_event, on_presence, NULL), "the subscriber starts");
    CHECK(wait_connected(3000), "and reports itself connected");

    FriendEvent e = {
        .type         = FRIEND_EVENT_LIST_READY,
        .account_id   = 4242,
        .character_id = 7,
        .peer_account = 0,
    };
    CHECK(friend_event_publish(world, &e), "an event is published");

    CHECK(wait_for(&g_rx.events, 1, 2000), "the subscriber receives it");

    pthread_mutex_lock(&g_rx.lock);
    FriendEvent got = g_rx.last_event;
    pthread_mutex_unlock(&g_rx.lock);

    CHECK(got.type == FRIEND_EVENT_LIST_READY, "with the right type");
    CHECK(got.account_id == 4242, "and the right account");
}

static void test_another_worlds_event_does_not_arrive(void) {
    printf("delivery: an event for a different world is not delivered here\n");

    pthread_mutex_lock(&g_rx.lock);
    int before = g_rx.events;
    pthread_mutex_unlock(&g_rx.lock);

    /* The whole reason there is a channel per world rather than one shared one
     * with an id field to filter on. */
    FriendEvent e = { .type = FRIEND_EVENT_LIST_READY, .account_id = 5555 };
    CHECK(friend_event_publish(9, &e), "an event is published for world 9");

    usleep(300000);

    pthread_mutex_lock(&g_rx.lock);
    int after = g_rx.events;
    pthread_mutex_unlock(&g_rx.lock);

    CHECK(after == before, "the world-4 subscriber did not see it");
}

static void test_presence_changes_reach_every_world(void) {
    printf("delivery: a presence change reaches a world that never asked for it\n");

    pthread_mutex_lock(&g_rx.lock);
    int before = g_rx.presences;
    pthread_mutex_unlock(&g_rx.lock);

    /* Presence is one shared channel: the publisher has no idea who is watching
     * whom, so every world hears every change and filters locally. */
    PresenceRecord rec = { .account_id = 31337, .world_id = 2, .online = 1, .since = time(NULL) };
    snprintf(rec.character_name, sizeof(rec.character_name), "%s", "Elsewhere");
    CHECK(presence_publish(&rec), "a presence change is published");

    CHECK(wait_for(&g_rx.presences, before + 1, 2000), "the subscriber receives it");

    pthread_mutex_lock(&g_rx.lock);
    PresenceRecord got = g_rx.last_presence;
    pthread_mutex_unlock(&g_rx.lock);

    CHECK(got.account_id == 31337, "naming the right account");
    CHECK(strcmp(got.character_name, "Elsewhere") == 0, "and the right character");
}

static void test_publishing_to_world_zero_is_refused(void) {
    printf("delivery: an event with no world to go to is refused\n");

    FriendEvent e = { .type = FRIEND_EVENT_RESULT, .account_id = 1 };
    CHECK(!friend_event_publish(0, &e), "world 0 is refused rather than broadcast");
}

static void test_the_bus_stops_cleanly(void) {
    printf("delivery: the subscriber stops on request\n");

    friend_bus_stop();
    CHECK(!friend_bus_connected(), "it reports itself disconnected");

    /* Stopping twice must not fault: shutdown paths run from signal handlers. */
    friend_bus_stop();
    CHECK(1, "and stopping again is harmless");
}

int main(void) {
    log_init();
    pthread_mutex_init(&g_rx.lock, NULL);

    printf("=== encoding ===\n");
    test_event_survives_a_round_trip();
    test_event_decode_refuses_rubbish();
    test_a_peer_name_cannot_forge_fields();

    printf("\n=== delivery ===\n");
    if (!session_init() || !session_is_ready()) {
        printf("  SKIP no reachable Redis; the encoding checks above still ran\n");
    } else {
        test_a_published_event_reaches_its_world();
        test_another_worlds_event_does_not_arrive();
        test_presence_changes_reach_every_world();
        test_publishing_to_world_zero_is_refused();
        test_the_bus_stops_cleanly();
    }

    friend_bus_stop();
    presence_consumer_close();
    session_close();

    if (g_failures) {
        printf("\n%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("\nAll friend bus checks passed\n");
    return 0;
}
