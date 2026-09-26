/**
 * @file
 * Check the presence bus: its wire encoding, and its behaviour against Redis.
 *
 * The encoding half runs everywhere. The Redis half needs a reachable server
 * and skips itself when there is not one, because a suite that fails on a
 * developer box without Redis teaches people to ignore it.
 *
 * What the encoding checks are really for: these messages cross process
 * boundaries between servers that are upgraded independently, and a character
 * name is the one field a player controls. A name containing the field
 * separator must not be able to forge the fields around it.
 */

#include "presence.h"
#include "session.h"
#include "log.h"

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

/* --- Encoding ------------------------------------------------------------ */

static void test_mutation_survives_a_round_trip(void) {
    printf("encode: a mutation round-trips\n");

    FriendMutation in = {
        .op                 = FRIEND_OP_REQUEST,
        .actor_account      = 41,
        .target_account     = 0,
        .origin_world_id    = 3,
        .actor_character_id = 900,
    };
    snprintf(in.target_name, sizeof(in.target_name), "%s", "Kaelen");

    char buf[256];
    size_t n = friend_mutation_encode(&in, buf, sizeof(buf));
    CHECK(n > 0, "it encodes");

    FriendMutation out;
    CHECK(friend_mutation_decode(buf, &out), "it decodes");
    CHECK(out.op == in.op, "the op survives");
    CHECK(out.actor_account == in.actor_account, "the actor survives");
    CHECK(out.target_account == in.target_account, "the target survives");
    CHECK(out.origin_world_id == in.origin_world_id, "the origin world survives");
    CHECK(out.actor_character_id == in.actor_character_id, "the character survives");
    CHECK(strcmp(out.target_name, in.target_name) == 0, "the name survives");
}

static void test_a_name_cannot_forge_the_fields_around_it(void) {
    printf("encode: a separator inside a name does not split the message\n");

    FriendMutation in = {
        .op              = FRIEND_OP_REMOVE,
        .actor_account   = 7,
        .target_account  = 8,
        .origin_world_id = 1,
    };
    /* A player who can name a character this must not be able to rewrite the
     * numeric fields of a message about them. */
    snprintf(in.target_name, sizeof(in.target_name), "%s", "Evil|9|9|9|9|x");

    char buf[256];
    CHECK(friend_mutation_encode(&in, buf, sizeof(buf)) > 0, "it encodes");

    FriendMutation out;
    CHECK(friend_mutation_decode(buf, &out), "it decodes");
    CHECK(out.actor_account == 7, "the actor is untouched");
    CHECK(out.target_account == 8, "the target is untouched");
    CHECK(strcmp(out.target_name, "Evil|9|9|9|9|x") == 0,
          "and the name comes back whole, separators and all");
}

static void test_mutation_decode_refuses_rubbish(void) {
    printf("encode: a decode of nonsense fails rather than half-filling\n");

    FriendMutation out;
    CHECK(!friend_mutation_decode("", &out), "an empty string is refused");
    CHECK(!friend_mutation_decode("nonsense", &out), "an unstructured string is refused");
    CHECK(!friend_mutation_decode("0|1", &out), "a truncated message is refused");
    CHECK(!friend_mutation_decode("999|1|2|3|4|x", &out), "an unknown op is refused");
    CHECK(!friend_mutation_decode("-1|1|2|3|4|x", &out), "a negative op is refused");
}

static void test_mutation_encode_refuses_a_short_buffer(void) {
    printf("encode: a buffer too small is refused, not truncated\n");

    FriendMutation in = { .op = FRIEND_OP_REQUEST, .actor_account = 4242424 };
    snprintf(in.target_name, sizeof(in.target_name), "%s", "AVeryLongCharacterNameIndeed");

    char tiny[8];
    CHECK(friend_mutation_encode(&in, tiny, sizeof(tiny)) == 0,
          "encoding into a short buffer reports failure");
}

static void test_presence_survives_a_round_trip(void) {
    printf("encode: a presence change round-trips\n");

    PresenceRecord in = {
        .account_id   = 12,
        .world_id     = 5,
        .character_id = 88,
        .since        = 1700000000,
        .online       = 1,
    };
    snprintf(in.character_name, sizeof(in.character_name), "%s", "Bramble");

    char buf[256];
    CHECK(presence_encode(&in, buf, sizeof(buf)) > 0, "it encodes");

    PresenceRecord out;
    CHECK(presence_decode(buf, &out), "it decodes");
    CHECK(out.account_id == 12, "the account survives");
    CHECK(out.world_id == 5, "the world survives");
    CHECK(out.character_id == 88, "the character survives");
    CHECK(out.since == 1700000000, "the timestamp survives");
    CHECK(out.online == 1, "the online flag survives");
    CHECK(strcmp(out.character_name, "Bramble") == 0, "the name survives");
}

static void test_offline_presence_round_trips(void) {
    printf("encode: an offline change round-trips\n");

    PresenceRecord in = { .account_id = 12, .online = 0 };

    char buf[256];
    CHECK(presence_encode(&in, buf, sizeof(buf)) > 0, "it encodes");

    PresenceRecord out;
    CHECK(presence_decode(buf, &out), "it decodes");
    CHECK(out.account_id == 12, "the account survives");
    CHECK(out.online == 0, "and it still says offline");
}

static void test_presence_decode_refuses_rubbish(void) {
    printf("encode: a presence decode of nonsense fails\n");

    PresenceRecord out;
    CHECK(!presence_decode("", &out), "an empty string is refused");
    CHECK(!presence_decode("1|2", &out), "a truncated message is refused");
}

static void test_a_long_name_is_bounded_not_overrun(void) {
    printf("encode: an over-long name is truncated into the field, not past it\n");

    /* The wire is not obliged to carry a name longer than the struct holds, but
     * it must never write one past the end of it. */
    char forged[512];
    snprintf(forged, sizeof(forged), "0|1|2|3|4|");
    memset(forged + strlen(forged), 'A', 200);
    forged[strlen("0|1|2|3|4|") + 200] = '\0';

    FriendMutation out;
    CHECK(friend_mutation_decode(forged, &out), "an over-long name still decodes");
    CHECK(strlen(out.target_name) == PRESENCE_NAME_LEN - 1,
          "and is clamped to the field width");
}

/* --- Redis --------------------------------------------------------------- */

static int redis_ready(void) {
    return session_is_ready();
}

static void test_presence_set_get_and_clear(void) {
    printf("redis: presence is written, read and cleared\n");

    const uint32_t account = 990001;
    presence_clear(account);

    CHECK(presence_set(account, 4, 77, "Thornwood"), "presence is written");

    PresenceRecord rec;
    CHECK(presence_get(account, &rec) == 1, "and reads back as online");
    CHECK(rec.world_id == 4, "with the right world");
    CHECK(rec.character_id == 77, "the right character");
    CHECK(strcmp(rec.character_name, "Thornwood") == 0, "and the right name");

    CHECK(presence_clear(account), "presence is cleared");
    CHECK(presence_get(account, &rec) == 0, "and reads back as offline");
}

/* A world transfer is a leave and an enter racing across two processes: the
 * player is already on world B when world A finally gets around to tidying up.
 * An unconditional DEL there wipes the presence B just wrote, and because the
 * heartbeat only EXPIREs an existing key, nothing puts it back -- the player
 * reads as offline to every friend for the rest of their session. */
static void test_a_stale_leave_does_not_wipe_a_newer_presence(void) {
    printf("redis: a leave from the old world spares the presence of the new one\n");

    const uint32_t account = 990030;
    presence_clear(account);

    /* The player is on world 8 as character 501. */
    CHECK(presence_set(account, 8, 501, "Newworld"), "the new world writes presence");

    /* World 3 now processes the disconnect of the character they left behind. */
    CHECK(presence_clear_if_character(account, 500) == 0,
          "the stale leave clears nothing");

    PresenceRecord rec;
    CHECK(presence_get(account, &rec) == 1, "the player is still online");
    CHECK(rec.world_id == 8, "on the world they moved to");
    CHECK(rec.character_id == 501, "as the character they moved as");

    /* And the matching leave still works, or nobody would ever go offline. */
    CHECK(presence_clear_if_character(account, 501) == 1,
          "the matching leave does clear");
    CHECK(presence_get(account, &rec) == 0, "and the player reads as offline");
}

static void test_presence_get_many_aligns_with_its_input(void) {
    printf("redis: a batched read stays aligned with the ids asked for\n");

    const uint32_t ids[3] = { 990010, 990011, 990012 };
    for (int i = 0; i < 3; i++) presence_clear(ids[i]);

    /* Deliberately only the middle one online: the alignment bug this guards
     * against is a reader that packs found records and leaves the caller
     * matching the wrong name to the wrong friend. */
    presence_set(ids[1], 2, 33, "Middle");

    PresenceRecord recs[3];
    int online = presence_get_many(ids, 3, recs);

    CHECK(online == 1, "one of the three is online");
    CHECK(recs[0].online == 0, "the first is offline");
    CHECK(recs[1].online == 1, "the second is online");
    CHECK(recs[2].online == 0, "the third is offline");
    CHECK(recs[1].account_id == ids[1], "and the online record names the right account");
    CHECK(strcmp(recs[1].character_name, "Middle") == 0, "with the right character");

    for (int i = 0; i < 3; i++) presence_clear(ids[i]);
}

static void test_friends_cache_round_trips(void) {
    printf("redis: the friends cache stores and reloads a set\n");

    const uint32_t owner = 990020;
    const FriendCacheEntry friends[3] = {
        { 5, "Thornwood" },
        { 6, "Ashvale" },
        { 7, "Brightmoor" },
    };

    presence_friends_drop(owner);
    CHECK(presence_friends_load(owner, NULL, 0) == -1, "an uncached account misses");

    CHECK(presence_friends_store(owner, friends, 3), "the set is stored");

    FriendCacheEntry out[8];
    int n = presence_friends_load(owner, out, 8);
    CHECK(n == 3, "three friends come back");

    int seen = 0;
    for (int i = 0; i < n; i++)
        for (int j = 0; j < 3; j++)
            if (out[i].account_id == friends[j].account_id
                && strcmp(out[i].name, friends[j].name) == 0) seen++;
    CHECK(seen == 3, "each with the name it was stored under");

    presence_friends_drop(owner);
}

/* The name is what the panel labels an offline friend with, and it is chosen by
 * the player who owns it. A space would split the SADD into two arguments if
 * the command were built by hand; a separator would forge the id in front of it
 * if the decoder looked for one after the name. */
static void test_a_friend_name_survives_being_hostile(void) {
    printf("redis: a friend name with spaces and separators round-trips intact\n");

    const uint32_t owner = 990022;
    const FriendCacheEntry friends[2] = {
        { 11, "Two Words" },
        { 12, "Evil|99|x" },
    };

    presence_friends_drop(owner);
    CHECK(presence_friends_store(owner, friends, 2), "both are stored");

    FriendCacheEntry out[4];
    int n = presence_friends_load(owner, out, 4);
    CHECK(n == 2, "both come back");

    int spaced = 0, separated = 0;
    for (int i = 0; i < n; i++) {
        if (out[i].account_id == 11 && strcmp(out[i].name, "Two Words") == 0) spaced = 1;
        /* The id in front is the stored one, not the 99 buried in the name. */
        if (out[i].account_id == 12 && strcmp(out[i].name, "Evil|99|x") == 0) separated = 1;
    }
    CHECK(spaced, "a name with a space is one member, intact");
    CHECK(separated, "a name with separators cannot forge the id in front of it");

    presence_friends_drop(owner);
}

static void test_an_empty_friends_set_is_cached_not_missing(void) {
    printf("redis: a friendless account caches as empty, not as a miss\n");

    const uint32_t owner = 990021;
    presence_friends_drop(owner);

    CHECK(presence_friends_store(owner, NULL, 0), "an empty set is stored");

    FriendCacheEntry out[4];
    int n = presence_friends_load(owner, out, 4);
    CHECK(n == 0, "it loads as empty");
    CHECK(n != -1, "and specifically not as a cache miss");

    presence_friends_drop(owner);
}

static void test_the_mutation_queue_is_first_in_first_out(void) {
    printf("redis: mutations come off the queue in the order they went on\n");

    /* Drain anything a previous run left behind. */
    FriendMutation drained;
    while (friend_mutation_pop(&drained, 0) == 1) { }

    FriendMutation first  = { .op = FRIEND_OP_REQUEST, .actor_account = 1, .target_account = 2 };
    FriendMutation second = { .op = FRIEND_OP_REMOVE,  .actor_account = 3, .target_account = 4 };

    CHECK(friend_mutation_push(&first), "the first is queued");
    CHECK(friend_mutation_push(&second), "the second is queued");

    FriendMutation got;
    CHECK(friend_mutation_pop(&got, 1) == 1, "one comes back");
    CHECK(got.actor_account == 1 && got.op == FRIEND_OP_REQUEST, "and it is the first one");

    CHECK(friend_mutation_pop(&got, 1) == 1, "the other comes back");
    CHECK(got.actor_account == 3 && got.op == FRIEND_OP_REMOVE, "and it is the second one");

    CHECK(friend_mutation_pop(&got, 0) == 0, "and then the queue is empty");
}

static void test_a_queued_mutation_outlives_the_consumer(void) {
    printf("redis: a mutation queued with nobody listening is still there later\n");

    FriendMutation drained;
    while (friend_mutation_pop(&drained, 0) == 1) { }

    /* This is the whole reason mutations are a list and not a publish. */
    FriendMutation m = { .op = FRIEND_OP_ACCEPT, .actor_account = 55, .target_account = 66 };
    CHECK(friend_mutation_push(&m), "it is queued while nothing is consuming");

    presence_consumer_close();          /* the consumer goes away entirely */

    FriendMutation got;
    CHECK(friend_mutation_pop(&got, 1) == 1, "a new consumer still finds it");
    CHECK(got.actor_account == 55, "intact");
}


static void test_the_request_cache_round_trips(void) {
    printf("redis: pending requests cache and reload in order\n");

    const uint32_t owner = 990040;
    presence_requests_drop(owner);
    CHECK(presence_requests_load(owner, NULL, 0) == -1, "an uncached account misses");

    FriendRequestCache in[2] = {
        { .from_account = 11, .created_at = 1700000000 },
        { .from_account = 22, .created_at = 1700000500 },
    };
    snprintf(in[0].from_name, sizeof(in[0].from_name), "%s", "Older");
    snprintf(in[1].from_name, sizeof(in[1].from_name), "%s", "Newer");

    CHECK(presence_requests_store(owner, in, 2), "two requests are stored");

    FriendRequestCache out[4];
    int n = presence_requests_load(owner, out, 4);
    CHECK(n == 2, "two come back");
    CHECK(n == 2 && out[0].from_account == 11, "oldest first");
    CHECK(n == 2 && out[1].from_account == 22, "then the newer one");
    CHECK(n == 2 && strcmp(out[0].from_name, "Older") == 0, "carrying their names");
    CHECK(n == 2 && out[1].created_at == 1700000500, "and their timestamps");

    presence_requests_drop(owner);
}

static void test_an_empty_request_cache_is_not_a_miss(void) {
    printf("redis: no pending requests caches as empty, not as a miss\n");

    const uint32_t owner = 990041;
    presence_requests_drop(owner);

    CHECK(presence_requests_store(owner, NULL, 0), "an empty set is stored");

    FriendRequestCache out[2];
    int n = presence_requests_load(owner, out, 2);
    CHECK(n == 0, "it loads as empty");
    CHECK(n != -1, "and specifically not as a cache miss");

    presence_requests_drop(owner);
}

int main(void) {
    log_init();

    printf("=== encoding ===\n");
    test_mutation_survives_a_round_trip();
    test_a_name_cannot_forge_the_fields_around_it();
    test_mutation_decode_refuses_rubbish();
    test_mutation_encode_refuses_a_short_buffer();
    test_presence_survives_a_round_trip();
    test_offline_presence_round_trips();
    test_presence_decode_refuses_rubbish();
    test_a_long_name_is_bounded_not_overrun();

    printf("\n=== redis ===\n");
    if (!session_init() || !redis_ready()) {
        printf("  SKIP no reachable Redis; the encoding checks above still ran\n");
    } else {
        test_presence_set_get_and_clear();
        test_presence_get_many_aligns_with_its_input();
        test_a_stale_leave_does_not_wipe_a_newer_presence();
        test_friends_cache_round_trips();
        test_a_friend_name_survives_being_hostile();
        test_an_empty_friends_set_is_cached_not_missing();
        test_the_mutation_queue_is_first_in_first_out();
        test_a_queued_mutation_outlives_the_consumer();
        test_the_request_cache_round_trips();
        test_an_empty_request_cache_is_not_a_miss();
    }

    presence_consumer_close();
    session_close();

    if (g_failures) {
        printf("\n%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("\nAll presence checks passed\n");
    return 0;
}
