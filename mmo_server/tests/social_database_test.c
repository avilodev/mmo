/**
 * @file
 * Check the friend graph's decision table, its caps, and its transactions.
 *
 * What this suite pins down is the part that is invisible when it breaks. A
 * friendship is two directed rows, and each side reads its own direction with a
 * single-direction scan -- so a half-landed accept leaves A friends with B
 * while B is not friends with A, and nothing afterwards notices. Every mutation
 * that writes more than one row is therefore checked from BOTH sides.
 *
 * Runs against a real SQLite file in a temporary directory, not a mock: the
 * transactions are the thing under test and an in-memory fake would not have
 * them.
 */

#include "social_database.h"
#include "log.h"

#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
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

static char g_db_path[512];

/** Open a database file private to this run, replacing any previous one. */
static void fresh_db(void) {
    social_db_close();
    unlink(g_db_path);
    if (!social_db_init(g_db_path)) {
        printf("  FATAL could not open %s\n", g_db_path);
        exit(1);
    }
}

/** Report whether one directed friendship row exists.
 *
 * Reads the table directly rather than through social_is_friend(), so a
 * one-directional write cannot be hidden by a reader that happens to check the
 * same direction it was written in.
 */
static int row_exists(const char* sql, uint32_t a, uint32_t b) {
    sqlite3* db = NULL;
    if (sqlite3_open(g_db_path, &db) != SQLITE_OK) return -1;

    sqlite3_stmt* stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return -1;
    }
    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)a);
    sqlite3_bind_int64(stmt, 2, (sqlite3_int64)b);

    int found = (sqlite3_step(stmt) == SQLITE_ROW) ? sqlite3_column_int(stmt, 0) : -1;
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return found;
}

static int friendship_row(uint32_t owner, uint32_t friend_id) {
    return row_exists("SELECT COUNT(*) FROM friendships "
                      "WHERE owner_account = ? AND friend_account = ?;",
                      owner, friend_id);
}

static int request_row(uint32_t from_account, uint32_t to_account) {
    return row_exists("SELECT COUNT(*) FROM friend_requests "
                      "WHERE from_account = ? AND to_account = ?;",
                      from_account, to_account);
}

/** Backdate a pending request so the lazy sweep should collect it. */
static int backdate_request(uint32_t from_account, uint32_t to_account, int days) {
    sqlite3* db = NULL;
    if (sqlite3_open(g_db_path, &db) != SQLITE_OK) return 0;

    sqlite3_stmt* stmt = NULL;
    const char* sql = "UPDATE friend_requests SET created_at = ? "
                      "WHERE from_account = ? AND to_account = ?;";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return 0;
    }
    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)(time(NULL) - (int64_t)days * 86400));
    sqlite3_bind_int64(stmt, 2, (sqlite3_int64)from_account);
    sqlite3_bind_int64(stmt, 3, (sqlite3_int64)to_account);

    int ok = (sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return ok;
}

/* --- The decision table -------------------------------------------------- */

static void test_request_stores_and_is_visible_to_target(void) {
    printf("request: stored and visible to the target\n");
    fresh_db();

    CHECK(social_friend_request(1, 2) == FRIEND_RESULT_OK, "a fresh request is accepted");
    CHECK(request_row(1, 2) == 1, "the request row exists");

    FriendRequestEntry in[8];
    int n = social_friend_requests_incoming(2, in, 8);
    CHECK(n == 1, "the target sees one incoming request");
    CHECK(n == 1 && in[0].from_account == 1, "it names the sender");

    uint32_t out[8];
    int m = social_friend_requests_outgoing(1, out, 8);
    CHECK(m == 1 && out[0] == 2, "the sender sees one outgoing request");

    CHECK(social_is_friend(1, 2) == 0, "a request alone is not a friendship");
}

static void test_duplicate_request_is_reported_not_duplicated(void) {
    printf("request: a repeat is reported, not stored twice\n");
    fresh_db();

    social_friend_request(1, 2);
    CHECK(social_friend_request(1, 2) == FRIEND_RESULT_ALREADY_PENDING,
          "the second request reports already-pending");
    CHECK(request_row(1, 2) == 1, "still exactly one request row");
}

static void test_self_request_is_rejected(void) {
    printf("request: nobody friends themselves\n");
    fresh_db();

    CHECK(social_friend_request(7, 7) == FRIEND_RESULT_SELF, "a self-request is refused");
    CHECK(request_row(7, 7) == 0, "and nothing is written");
}

static void test_request_to_an_existing_friend_is_reported(void) {
    printf("request: an existing friend is reported, not re-requested\n");
    fresh_db();

    social_friend_request(1, 2);
    social_friend_accept(2, 1);

    CHECK(social_friend_request(1, 2) == FRIEND_RESULT_ALREADY_FRIENDS,
          "requesting an existing friend reports already-friends");
    CHECK(request_row(1, 2) == 0, "and writes no request row");
}

static void test_crossing_requests_collapse_into_a_friendship(void) {
    printf("request: two crossing requests are two consents\n");
    fresh_db();

    CHECK(social_friend_request(1, 2) == FRIEND_RESULT_OK, "A requests B");
    CHECK(social_friend_request(2, 1) == FRIEND_RESULT_MUTUAL,
          "B requesting A back collapses to an accept");

    CHECK(friendship_row(1, 2) == 1, "A's direction is written");
    CHECK(friendship_row(2, 1) == 1, "B's direction is written");
    CHECK(request_row(1, 2) == 0, "A's request row is gone");
    CHECK(request_row(2, 1) == 0, "B's request row is gone");
}

/* --- Accept, decline, cancel, remove ------------------------------------- */

static void test_accept_writes_both_directions(void) {
    printf("accept: writes both directions and clears the request\n");
    fresh_db();

    social_friend_request(1, 2);
    CHECK(social_friend_accept(2, 1) == FRIEND_RESULT_OK, "the target accepts");

    CHECK(friendship_row(1, 2) == 1, "the sender's direction exists");
    CHECK(friendship_row(2, 1) == 1, "the accepter's direction exists");
    CHECK(request_row(1, 2) == 0, "the request row is consumed");
    CHECK(social_is_friend(1, 2) == 1, "the sender reads as a friend");
    CHECK(social_is_friend(2, 1) == 1, "the accepter reads as a friend");
}

static void test_accept_without_a_request_is_refused(void) {
    printf("accept: an accept with no request behind it writes nothing\n");
    fresh_db();

    CHECK(social_friend_accept(2, 1) == FRIEND_RESULT_NOT_FOUND,
          "accepting a request that does not exist is refused");
    CHECK(friendship_row(1, 2) == 0, "no friendship appears");
    CHECK(friendship_row(2, 1) == 0, "in either direction");
}

static void test_decline_removes_the_request_only(void) {
    printf("decline: the request goes, the sender is not told\n");
    fresh_db();

    social_friend_request(1, 2);
    CHECK(social_friend_decline(2, 1) == FRIEND_RESULT_OK, "the target declines");
    CHECK(request_row(1, 2) == 0, "the request row is gone");
    CHECK(social_is_friend(1, 2) == 0, "and no friendship was made");
}

static void test_cancel_withdraws_an_outgoing_request(void) {
    printf("cancel: the sender withdraws\n");
    fresh_db();

    social_friend_request(1, 2);
    CHECK(social_friend_cancel(1, 2) == FRIEND_RESULT_OK, "the sender cancels");
    CHECK(request_row(1, 2) == 0, "the request row is gone");
}

static void test_remove_is_unilateral_and_deletes_both_rows(void) {
    printf("remove: one side leaving deletes both directions\n");
    fresh_db();

    social_friend_request(1, 2);
    social_friend_accept(2, 1);

    CHECK(social_friend_remove(1, 2) == FRIEND_RESULT_OK, "A removes B");
    CHECK(friendship_row(1, 2) == 0, "A's direction is gone");
    CHECK(friendship_row(2, 1) == 0, "B's direction is gone too, unasked");
}

static void test_remove_of_a_non_friend_is_refused(void) {
    printf("remove: removing a stranger is refused\n");
    fresh_db();

    CHECK(social_friend_remove(1, 2) == FRIEND_RESULT_NOT_FOUND,
          "removing somebody who is not a friend reports not-found");
}

/* --- Blocking ------------------------------------------------------------ */

static void test_block_clears_the_relationship_in_both_directions(void) {
    printf("block: clears friendship and requests both ways\n");
    fresh_db();

    social_friend_request(1, 2);
    social_friend_accept(2, 1);
    social_friend_request(3, 2);

    CHECK(social_block_add(2, 1) == FRIEND_RESULT_OK, "B blocks A");
    CHECK(friendship_row(1, 2) == 0, "A's friendship row is gone");
    CHECK(friendship_row(2, 1) == 0, "B's friendship row is gone");
    CHECK(social_is_blocked(2, 1) == 1, "the block is recorded");
    CHECK(social_is_blocked(1, 2) == 0, "and is not symmetric");
    CHECK(request_row(3, 2) == 1, "an unrelated request is untouched");
}

static void test_a_blocked_request_is_swallowed(void) {
    printf("block: a request from a blocked account writes nothing\n");
    fresh_db();

    social_block_add(2, 1);
    CHECK(social_friend_request(1, 2) == FRIEND_RESULT_BLOCKED,
          "the request reports blocked to its caller");
    CHECK(request_row(1, 2) == 0, "and no row is written");
}

static void test_unblock_lets_requests_through_again(void) {
    printf("block: lifting a block reopens the path\n");
    fresh_db();

    social_block_add(2, 1);
    CHECK(social_block_remove(2, 1) == FRIEND_RESULT_OK, "B unblocks A");
    CHECK(social_is_blocked(2, 1) == 0, "the block is gone");
    CHECK(social_friend_request(1, 2) == FRIEND_RESULT_OK, "a request lands again");
}

/* --- Caps ---------------------------------------------------------------- */

static void test_friend_cap_is_enforced(void) {
    printf("caps: a full list refuses a new friendship\n");
    fresh_db();

    /* Fill account 1 to exactly MAX_FRIENDS through the real accept path, so
     * the cap is measured against rows the module wrote itself. */
    for (int i = 0; i < MAX_FRIENDS; i++) {
        uint32_t other = (uint32_t)(1000 + i);
        social_friend_request(other, 1);
        social_friend_accept(1, other);
    }
    CHECK(social_friend_count(1) == MAX_FRIENDS, "the list is full");

    CHECK(social_friend_request(1, 2) == FRIEND_RESULT_FRIEND_CAP,
          "a full sender cannot request");

    social_friend_request(2, 3);         /* an unrelated pair still works */
    CHECK(request_row(2, 3) == 1, "an unrelated request is unaffected");

    /* And a full TARGET refuses too, or the cap is only half a cap. */
    fresh_db();
    for (int i = 0; i < MAX_FRIENDS; i++) {
        uint32_t other = (uint32_t)(1000 + i);
        social_friend_request(other, 5);
        social_friend_accept(5, other);
    }
    CHECK(social_friend_request(9, 5) == FRIEND_RESULT_FRIEND_CAP,
          "a full target cannot be requested");
}

static void test_pending_cap_is_enforced(void) {
    printf("caps: outgoing requests are budgeted\n");
    fresh_db();

    for (int i = 0; i < MAX_PENDING_REQUESTS; i++)
        social_friend_request(1, (uint32_t)(2000 + i));

    uint32_t out[MAX_PENDING_REQUESTS + 4];
    CHECK(social_friend_requests_outgoing(1, out, MAX_PENDING_REQUESTS + 4)
              == MAX_PENDING_REQUESTS,
          "the budget is spent");

    CHECK(social_friend_request(1, 3000) == FRIEND_RESULT_PENDING_CAP,
          "the next request is refused with a reason");
    CHECK(request_row(1, 3000) == 0, "and is not silently stored");
}

/* --- Expiry -------------------------------------------------------------- */

static void test_stale_requests_are_swept_on_read(void) {
    printf("expiry: a request older than the TTL is swept when read\n");
    fresh_db();

    social_friend_request(1, 2);
    social_friend_request(3, 2);
    CHECK(backdate_request(1, 2, FRIEND_REQUEST_TTL_DAYS + 1), "one request is backdated");

    FriendRequestEntry in[8];
    int n = social_friend_requests_incoming(2, in, 8);
    CHECK(n == 1, "only the fresh request is returned");
    CHECK(n == 1 && in[0].from_account == 3, "and it is the fresh one");
    CHECK(request_row(1, 2) == 0, "the stale row is actually deleted");
}

/* --- Reads --------------------------------------------------------------- */

static void test_friend_list_carries_last_seen_details(void) {
    printf("read: the list renders an offline friend without a world database\n");
    fresh_db();

    social_friend_request(1, 2);
    social_friend_accept(2, 1);
    CHECK(social_presence_update(2, 4, 55, "Kaelen"), "B's last-seen row is written");

    FriendEntry list[8];
    int n = social_friend_list(1, list, 8);
    CHECK(n == 1, "A has one friend");
    CHECK(n == 1 && list[0].account_id == 2, "it is B");
    CHECK(n == 1 && list[0].last_world_id == 4, "carrying the world B was last in");
    CHECK(n == 1 && strcmp(list[0].last_character_name, "Kaelen") == 0,
          "and the character name B was last seen as");
    CHECK(n == 1 && list[0].last_seen_at > 0, "and when");
}

static void test_friend_list_tolerates_a_friend_never_seen(void) {
    printf("read: a friend who has never entered a world still lists\n");
    fresh_db();

    social_friend_request(1, 2);
    social_friend_accept(2, 1);

    FriendEntry list[8];
    int n = social_friend_list(1, list, 8);
    CHECK(n == 1, "the friend is listed");
    CHECK(n == 1 && list[0].last_world_id == 0, "with no world");
    CHECK(n == 1 && list[0].last_character_name[0] == '\0', "and no name");
}

static void test_friend_list_respects_the_caller_buffer(void) {
    printf("read: a short buffer truncates rather than overruns\n");
    fresh_db();

    for (int i = 0; i < 5; i++) {
        uint32_t other = (uint32_t)(4000 + i);
        social_friend_request(other, 1);
        social_friend_accept(1, other);
    }

    FriendEntry list[3];
    int n = social_friend_list(1, list, 3);
    CHECK(n == 3, "exactly the buffer's worth is returned");
}

static void test_presence_update_replaces_rather_than_accumulates(void) {
    printf("read: last-seen is one row per account\n");
    fresh_db();

    social_presence_update(9, 1, 10, "First");
    social_presence_update(9, 7, 11, "Second");

    social_friend_request(9, 1);
    social_friend_accept(1, 9);

    FriendEntry list[4];
    int n = social_friend_list(1, list, 4);
    CHECK(n == 1, "the friend appears once");
    CHECK(n == 1 && list[0].last_world_id == 7, "carrying the newer world");
    CHECK(n == 1 && strcmp(list[0].last_character_name, "Second") == 0,
          "and the newer name");
}

/* --- Durability ---------------------------------------------------------- */

static void test_the_graph_survives_a_reopen(void) {
    printf("durability: the graph is still there after a close and reopen\n");
    fresh_db();

    social_friend_request(1, 2);
    social_friend_accept(2, 1);
    social_block_add(1, 3);

    social_db_close();
    CHECK(social_db_init(g_db_path), "the database reopens");

    CHECK(social_is_friend(1, 2) == 1, "the friendship survived");
    CHECK(social_is_friend(2, 1) == 1, "in both directions");
    CHECK(social_is_blocked(1, 3) == 1, "and so did the block");
}


/* --- Name resolution ----------------------------------------------------- */

static void test_a_name_resolves_to_its_account(void) {
    printf("names: a character name resolves to the account behind it\n");
    fresh_db();

    social_presence_update(42, 3, 100, "Kaelen");

    uint32_t account = 0;
    CHECK(social_account_by_character_name("Kaelen", 0, &account), "the name resolves");
    CHECK(account == 42, "to the right account");
}

static void test_an_unknown_name_resolves_to_nothing(void) {
    printf("names: a name nobody has ever used resolves to nothing\n");
    fresh_db();

    uint32_t account = 12345;
    CHECK(!social_account_by_character_name("Nobody", 0, &account),
          "an unknown name is refused");
    CHECK(account == 0, "and the out parameter is cleared rather than left stale");
}

static void test_the_asking_world_breaks_a_tie(void) {
    printf("names: the same name on two worlds resolves to the asker's world\n");
    fresh_db();

    /* Legitimate: character names are UNIQUE(name, world_id), so this is two
     * different people who both chose the same name on different worlds. */
    social_presence_update(10, 1, 500, "Bramble");
    social_presence_update(20, 7, 600, "Bramble");

    uint32_t account = 0;
    CHECK(social_account_by_character_name("Bramble", 7, &account),
          "a name shared across worlds resolves when a world is named");
    CHECK(account == 20, "to the one on the asking player's world");

    account = 0;
    CHECK(social_account_by_character_name("Bramble", 1, &account),
          "and the other way round too");
    CHECK(account == 10, "to the one on that world");
}

static void test_an_ambiguous_name_is_refused(void) {
    printf("names: an ambiguous name is refused rather than guessed\n");
    fresh_db();

    social_presence_update(10, 1, 500, "Bramble");
    social_presence_update(20, 7, 600, "Bramble");

    uint32_t account = 999;
    /* World 4 has no Bramble, and two other worlds do. Picking one of them
     * would send a friend request to a stranger. */
    CHECK(!social_account_by_character_name("Bramble", 4, &account),
          "two candidates and no tiebreak is refused");
    CHECK(account == 0, "and nothing is returned");
}

static void test_name_resolution_is_case_insensitive(void) {
    printf("names: case does not have to be typed exactly\n");
    fresh_db();

    social_presence_update(42, 3, 100, "Kaelen");

    uint32_t account = 0;
    CHECK(social_account_by_character_name("kaelen", 0, &account),
          "a lowercase spelling resolves");
    CHECK(account == 42, "to the same account");
}

static void test_name_resolution_refuses_empty_and_null(void) {
    printf("names: an empty or missing name is refused\n");
    fresh_db();

    social_presence_update(42, 3, 100, "Kaelen");

    uint32_t account = 1;
    CHECK(!social_account_by_character_name("", 0, &account), "an empty name is refused");
    CHECK(!social_account_by_character_name(NULL, 0, &account), "a null name is refused");
    CHECK(!social_account_by_character_name("Kaelen", 0, NULL),
          "a null out parameter is refused rather than dereferenced");
}

int main(void) {
    log_init();

    snprintf(g_db_path, sizeof(g_db_path), "/tmp/social_db_test_%d.sqlite", (int)getpid());

    test_request_stores_and_is_visible_to_target();
    test_duplicate_request_is_reported_not_duplicated();
    test_self_request_is_rejected();
    test_request_to_an_existing_friend_is_reported();
    test_crossing_requests_collapse_into_a_friendship();

    test_accept_writes_both_directions();
    test_accept_without_a_request_is_refused();
    test_decline_removes_the_request_only();
    test_cancel_withdraws_an_outgoing_request();
    test_remove_is_unilateral_and_deletes_both_rows();
    test_remove_of_a_non_friend_is_refused();

    test_block_clears_the_relationship_in_both_directions();
    test_a_blocked_request_is_swallowed();
    test_unblock_lets_requests_through_again();

    test_friend_cap_is_enforced();
    test_pending_cap_is_enforced();
    test_stale_requests_are_swept_on_read();

    test_friend_list_carries_last_seen_details();
    test_friend_list_tolerates_a_friend_never_seen();
    test_friend_list_respects_the_caller_buffer();
    test_presence_update_replaces_rather_than_accumulates();

    test_the_graph_survives_a_reopen();

    test_a_name_resolves_to_its_account();
    test_an_unknown_name_resolves_to_nothing();
    test_the_asking_world_breaks_a_tie();
    test_an_ambiguous_name_is_refused();
    test_name_resolution_is_case_insensitive();
    test_name_resolution_refuses_empty_and_null();

    social_db_close();
    unlink(g_db_path);

    if (g_failures) {
        printf("\n%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("\nAll social database checks passed\n");
    return 0;
}
