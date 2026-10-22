/**
 * @file
 * Store the account friend graph, its pending requests, and blocks in SQLite.
 *
 * Every function here takes g_social_lock for its whole body and every helper
 * below the lock is the unlocked variant of something public. That split is not
 * decoration: a mutation is several statements inside one transaction, and a
 * helper that took the lock itself would deadlock the first caller that used it.
 */
#include "social_database.h"
#include "log.h"

#include <pthread.h>
#include <sqlite3.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static sqlite3*       g_social_db   = NULL;
static pthread_mutex_t g_social_lock = PTHREAD_MUTEX_INITIALIZER;

/** How long a statement waits for a lock another connection holds.
 *
 * Matches users_database.c. The account database may have two handles open on
 * it -- the account table and this one -- and a friend write must wait out a
 * concurrent login rather than failing it.
 */
#define SOCIAL_BUSY_TIMEOUT_MS 5000

/* --- Schema ---------------------------------------------------------------
 *
 * `friendships` holds TWO directed rows per friendship, one per direction.
 * Reading your list is then a single indexed scan with no OR and no ordering
 * convention to remember. The cost is that both rows have to be written and
 * deleted together, which is what the transactions below are for.
 */
static const char* SOCIAL_SCHEMA =
    "CREATE TABLE IF NOT EXISTS friendships ("
    "  owner_account  INTEGER NOT NULL,"
    "  friend_account INTEGER NOT NULL,"
    "  created_at     INTEGER NOT NULL,"
    "  PRIMARY KEY(owner_account, friend_account)"
    ");"
    /* The UNIQUE is what makes a double-clicked button harmless instead of a
     * duplicate row. */
    "CREATE TABLE IF NOT EXISTS friend_requests ("
    "  from_account INTEGER NOT NULL,"
    "  to_account   INTEGER NOT NULL,"
    "  created_at   INTEGER NOT NULL,"
    "  UNIQUE(from_account, to_account)"
    ");"
    /* Reading your incoming requests scans by to_account, which the UNIQUE
     * above indexes the wrong way round for. */
    "CREATE INDEX IF NOT EXISTS idx_friend_requests_to"
    "  ON friend_requests(to_account);"
    "CREATE TABLE IF NOT EXISTS account_blocks ("
    "  account_id      INTEGER NOT NULL,"
    "  blocked_account INTEGER NOT NULL,"
    "  created_at      INTEGER NOT NULL,"
    "  PRIMARY KEY(account_id, blocked_account)"
    ");"
    "CREATE TABLE IF NOT EXISTS account_presence ("
    "  account_id          INTEGER PRIMARY KEY,"
    "  last_world_id       INTEGER NOT NULL,"
    "  last_character_id   INTEGER NOT NULL,"
    "  last_character_name TEXT    NOT NULL,"
    "  last_seen_at        INTEGER NOT NULL"
    ");"
    /* Every friend request resolves a typed name through this column, so it is
     * on the hot path of the only thing players do here often. NOCASE because
     * nobody types a name back with the capitalisation they read it in. */
    "CREATE INDEX IF NOT EXISTS idx_account_presence_name"
    "  ON account_presence(last_character_name COLLATE NOCASE);";

int social_db_init(const char* db_path) {
    if (!db_path) return 0;

    pthread_mutex_lock(&g_social_lock);

    if (g_social_db) {           /* idempotent: two services may both call this */
        pthread_mutex_unlock(&g_social_lock);
        return 1;
    }

    if (sqlite3_open(db_path, &g_social_db) != SQLITE_OK) {
        /* sqlite3_open allocates the handle even when it fails, and the error
         * string is read out of it -- so it is closed rather than abandoned,
         * and cleared, because every entry point treats non-NULL as open. */
        LOG_ERROR("Cannot open the social database %s: %s", db_path,
                  sqlite3_errmsg(g_social_db));
        sqlite3_close(g_social_db);
        g_social_db = NULL;
        pthread_mutex_unlock(&g_social_lock);
        return 0;
    }

    char* pragma_err = NULL;
    if (sqlite3_exec(g_social_db, "PRAGMA journal_mode=WAL;", NULL, NULL,
                     &pragma_err) != SQLITE_OK) {
        LOG_WARN("Could not enable WAL on %s: %s - falling back to the rollback journal",
                 db_path, pragma_err ? pragma_err : "unknown error");
        sqlite3_free(pragma_err);
        pragma_err = NULL;
    }
    if (sqlite3_busy_timeout(g_social_db, SOCIAL_BUSY_TIMEOUT_MS) != SQLITE_OK)
        LOG_WARN("Could not set a busy timeout on %s", db_path);

    char* err = NULL;
    if (sqlite3_exec(g_social_db, SOCIAL_SCHEMA, NULL, NULL, &err) != SQLITE_OK) {
        LOG_ERROR("Cannot create the social tables in %s: %s", db_path,
                  err ? err : "unknown error");
        sqlite3_free(err);
        sqlite3_close(g_social_db);
        g_social_db = NULL;
        pthread_mutex_unlock(&g_social_lock);
        return 0;
    }

    LOG_INFO("Social database initialized: %s", db_path);
    pthread_mutex_unlock(&g_social_lock);
    return 1;
}

void social_db_close(void) {
    pthread_mutex_lock(&g_social_lock);
    if (g_social_db) {
        sqlite3_close(g_social_db);
        g_social_db = NULL;
    }
    pthread_mutex_unlock(&g_social_lock);
}

/* --- Statement helpers, all unlocked ------------------------------------- */

/** Run one parameterless statement. */
static int exec_sql(const char* sql) {
    char* err = NULL;
    if (sqlite3_exec(g_social_db, sql, NULL, NULL, &err) == SQLITE_OK) return 1;

    LOG_ERROR("social: %s failed: %s", sql, err ? err : "unknown error");
    sqlite3_free(err);
    return 0;
}

/** Open a write transaction.
 *
 * BEGIN IMMEDIATE, not the default deferred BEGIN. A deferred transaction takes
 * its write lock at the first write, so two accepts racing can both pass their
 * SELECTs and then have one fail at COMMIT with SQLITE_BUSY -- after the caller
 * has already been told which branch of the decision table it is on. IMMEDIATE
 * takes the lock up front, so the loser waits out the busy timeout at BEGIN and
 * then reads the winner's committed state.
 */
static int tx_begin(void)    { return exec_sql("BEGIN IMMEDIATE;"); }
static int tx_commit(void)   { return exec_sql("COMMIT;"); }
static void tx_rollback(void) { (void)exec_sql("ROLLBACK;"); }

/** Run a two-parameter statement that returns no rows.
 *
 * @return Rows changed, or -1 on failure.
 */
static int exec_2(const char* sql, uint32_t a, uint32_t b) {
    sqlite3_stmt* stmt = NULL;
    if (sqlite3_prepare_v2(g_social_db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        LOG_ERROR("social: prepare failed: %s", sqlite3_errmsg(g_social_db));
        return -1;
    }
    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)a);
    sqlite3_bind_int64(stmt, 2, (sqlite3_int64)b);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        LOG_ERROR("social: step failed: %s", sqlite3_errmsg(g_social_db));
        return -1;
    }
    return sqlite3_changes(g_social_db);
}

/** Answer a two-parameter COUNT(*), or -1 on failure. */
static int count_2(const char* sql, uint32_t a, uint32_t b) {
    sqlite3_stmt* stmt = NULL;
    if (sqlite3_prepare_v2(g_social_db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        LOG_ERROR("social: prepare failed: %s", sqlite3_errmsg(g_social_db));
        return -1;
    }
    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)a);
    sqlite3_bind_int64(stmt, 2, (sqlite3_int64)b);

    int n = (sqlite3_step(stmt) == SQLITE_ROW) ? sqlite3_column_int(stmt, 0) : -1;
    sqlite3_finalize(stmt);
    return n;
}

/** Answer a one-parameter COUNT(*), or -1 on failure. */
static int count_1(const char* sql, uint32_t a) {
    sqlite3_stmt* stmt = NULL;
    if (sqlite3_prepare_v2(g_social_db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        LOG_ERROR("social: prepare failed: %s", sqlite3_errmsg(g_social_db));
        return -1;
    }
    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)a);

    int n = (sqlite3_step(stmt) == SQLITE_ROW) ? sqlite3_column_int(stmt, 0) : -1;
    sqlite3_finalize(stmt);
    return n;
}

static int friendship_exists(uint32_t owner, uint32_t other) {
    return count_2("SELECT COUNT(*) FROM friendships "
                   "WHERE owner_account = ? AND friend_account = ?;", owner, other) > 0;
}

static int request_exists(uint32_t from_account, uint32_t to_account) {
    return count_2("SELECT COUNT(*) FROM friend_requests "
                   "WHERE from_account = ? AND to_account = ?;",
                   from_account, to_account) > 0;
}

static int block_exists(uint32_t account, uint32_t other) {
    return count_2("SELECT COUNT(*) FROM account_blocks "
                   "WHERE account_id = ? AND blocked_account = ?;", account, other) > 0;
}

static int friend_count_unlocked(uint32_t account) {
    return count_1("SELECT COUNT(*) FROM friendships WHERE owner_account = ?;", account);
}

static int outgoing_count(uint32_t account) {
    return count_1("SELECT COUNT(*) FROM friend_requests WHERE from_account = ?;", account);
}

/** Insert both directed rows of a friendship. */
static int write_friendship(uint32_t a, uint32_t b) {
    const char* sql = "INSERT OR IGNORE INTO friendships "
                      "(owner_account, friend_account, created_at) VALUES (?, ?, ?);";
    int64_t now = (int64_t)time(NULL);

    for (int i = 0; i < 2; i++) {
        uint32_t owner  = i ? b : a;
        uint32_t friend_id = i ? a : b;

        sqlite3_stmt* stmt = NULL;
        if (sqlite3_prepare_v2(g_social_db, sql, -1, &stmt, NULL) != SQLITE_OK) {
            LOG_ERROR("social: prepare failed: %s", sqlite3_errmsg(g_social_db));
            return 0;
        }
        sqlite3_bind_int64(stmt, 1, (sqlite3_int64)owner);
        sqlite3_bind_int64(stmt, 2, (sqlite3_int64)friend_id);
        sqlite3_bind_int64(stmt, 3, (sqlite3_int64)now);

        int rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        if (rc != SQLITE_DONE) {
            LOG_ERROR("social: friendship insert failed: %s", sqlite3_errmsg(g_social_db));
            return 0;
        }
    }
    return 1;
}

/** Delete any request between two accounts, in both directions. */
static int drop_requests_both_ways(uint32_t a, uint32_t b) {
    return exec_2("DELETE FROM friend_requests WHERE "
                  "(from_account = ?1 AND to_account = ?2) OR "
                  "(from_account = ?2 AND to_account = ?1);", a, b) >= 0;
}

/** Delete requests to one account that have stood past the TTL.
 *
 * Scoped to the reader rather than sweeping the whole table: this runs on the
 * panel-open path, and a global sweep would make one player's click cost work
 * proportional to every pending request in the system.
 */
static int sweep_stale_requests(uint32_t to_account) {
    const char* sql = "DELETE FROM friend_requests WHERE to_account = ? AND created_at < ?;";
    sqlite3_stmt* stmt = NULL;

    if (sqlite3_prepare_v2(g_social_db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        LOG_ERROR("social: prepare failed: %s", sqlite3_errmsg(g_social_db));
        return 0;
    }
    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)to_account);
    sqlite3_bind_int64(stmt, 2,
        (sqlite3_int64)(time(NULL) - (int64_t)FRIEND_REQUEST_TTL_DAYS * 86400));

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

/* --- Mutations ----------------------------------------------------------- */

FriendResult social_friend_request(uint32_t from_account, uint32_t to_account) {
    if (from_account == to_account) return FRIEND_RESULT_SELF;
    if (from_account == 0 || to_account == 0) return FRIEND_RESULT_NOT_FOUND;

    pthread_mutex_lock(&g_social_lock);
    if (!g_social_db) {
        pthread_mutex_unlock(&g_social_lock);
        return FRIEND_RESULT_ERROR;
    }

    FriendResult result = FRIEND_RESULT_ERROR;

    if (!tx_begin()) {
        pthread_mutex_unlock(&g_social_lock);
        return FRIEND_RESULT_ERROR;
    }

    /* The decision table, in the order social_database.h documents. Every
     * branch that writes nothing rolls back rather than committing an empty
     * transaction, so the write lock is held for as little as possible. */
    if (block_exists(to_account, from_account)) {
        result = FRIEND_RESULT_BLOCKED;
    } else if (friendship_exists(from_account, to_account)) {
        result = FRIEND_RESULT_ALREADY_FRIENDS;
    } else if (request_exists(from_account, to_account)) {
        result = FRIEND_RESULT_ALREADY_PENDING;
    } else {
        int from_friends = friend_count_unlocked(from_account);
        int to_friends   = friend_count_unlocked(to_account);

        if (from_friends < 0 || to_friends < 0) {
            result = FRIEND_RESULT_ERROR;
        } else if (from_friends >= MAX_FRIENDS || to_friends >= MAX_FRIENDS) {
            result = FRIEND_RESULT_FRIEND_CAP;
        } else if (request_exists(to_account, from_account)) {
            /* Crossing requests. Sending a request is consent, so two of them
             * are two consents and this pair is already a friendship. Both
             * request rows go: leaving either behind would show both players an
             * invitation from somebody already on their list. */
            if (write_friendship(from_account, to_account) &&
                drop_requests_both_ways(from_account, to_account))
                result = FRIEND_RESULT_MUTUAL;
        } else {
            int pending = outgoing_count(from_account);
            if (pending < 0) {
                result = FRIEND_RESULT_ERROR;
            } else if (pending >= MAX_PENDING_REQUESTS) {
                result = FRIEND_RESULT_PENDING_CAP;
            } else {
                const char* sql = "INSERT INTO friend_requests "
                                  "(from_account, to_account, created_at) VALUES (?, ?, ?);";
                sqlite3_stmt* stmt = NULL;
                if (sqlite3_prepare_v2(g_social_db, sql, -1, &stmt, NULL) == SQLITE_OK) {
                    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)from_account);
                    sqlite3_bind_int64(stmt, 2, (sqlite3_int64)to_account);
                    sqlite3_bind_int64(stmt, 3, (sqlite3_int64)time(NULL));
                    if (sqlite3_step(stmt) == SQLITE_DONE) result = FRIEND_RESULT_OK;
                    sqlite3_finalize(stmt);
                }
            }
        }
    }

    if (result == FRIEND_RESULT_ERROR) tx_rollback();
    else if (!tx_commit())              result = FRIEND_RESULT_ERROR;

    pthread_mutex_unlock(&g_social_lock);
    return result;
}

FriendResult social_friend_accept(uint32_t to_account, uint32_t from_account) {
    if (to_account == from_account) return FRIEND_RESULT_SELF;

    pthread_mutex_lock(&g_social_lock);
    if (!g_social_db) {
        pthread_mutex_unlock(&g_social_lock);
        return FRIEND_RESULT_ERROR;
    }

    FriendResult result = FRIEND_RESULT_ERROR;

    if (!tx_begin()) {
        pthread_mutex_unlock(&g_social_lock);
        return FRIEND_RESULT_ERROR;
    }

    if (!request_exists(from_account, to_account)) {
        result = FRIEND_RESULT_NOT_FOUND;
    } else {
        int a = friend_count_unlocked(to_account);
        int b = friend_count_unlocked(from_account);

        if (a < 0 || b < 0) {
            result = FRIEND_RESULT_ERROR;
        } else if (a >= MAX_FRIENDS || b >= MAX_FRIENDS) {
            /* Either list filled while the request sat there. The request is
             * left standing: the cap may lift, and deleting it here would lose
             * a consent the sender already gave. */
            result = FRIEND_RESULT_FRIEND_CAP;
        } else if (write_friendship(to_account, from_account) &&
                   drop_requests_both_ways(to_account, from_account)) {
            result = FRIEND_RESULT_OK;
        }
    }

    if (result == FRIEND_RESULT_ERROR) tx_rollback();
    else if (!tx_commit())              result = FRIEND_RESULT_ERROR;

    pthread_mutex_unlock(&g_social_lock);
    return result;
}

/** Delete one directed request row and report whether it was there. */
static FriendResult drop_one_request(uint32_t from_account, uint32_t to_account) {
    pthread_mutex_lock(&g_social_lock);
    if (!g_social_db) {
        pthread_mutex_unlock(&g_social_lock);
        return FRIEND_RESULT_ERROR;
    }

    int changed = exec_2("DELETE FROM friend_requests "
                         "WHERE from_account = ? AND to_account = ?;",
                         from_account, to_account);
    pthread_mutex_unlock(&g_social_lock);

    if (changed < 0) return FRIEND_RESULT_ERROR;
    return changed ? FRIEND_RESULT_OK : FRIEND_RESULT_NOT_FOUND;
}

FriendResult social_friend_decline(uint32_t to_account, uint32_t from_account) {
    /* The sender is never told. That is a property of the caller, not of this
     * row: nothing here publishes anything, which is the point. */
    return drop_one_request(from_account, to_account);
}

FriendResult social_friend_cancel(uint32_t from_account, uint32_t to_account) {
    return drop_one_request(from_account, to_account);
}

FriendResult social_friend_remove(uint32_t account, uint32_t friend_account) {
    if (account == friend_account) return FRIEND_RESULT_SELF;

    pthread_mutex_lock(&g_social_lock);
    if (!g_social_db) {
        pthread_mutex_unlock(&g_social_lock);
        return FRIEND_RESULT_ERROR;
    }

    FriendResult result = FRIEND_RESULT_ERROR;

    if (!tx_begin()) {
        pthread_mutex_unlock(&g_social_lock);
        return FRIEND_RESULT_ERROR;
    }

    /* Both directions in one statement and one transaction. Removing is
     * unilateral, so the other side's row goes whether they agree or not --
     * and it must go in the same transaction, or the friendship survives
     * one-sidedly on exactly the side that did not ask to keep it. */
    int changed = exec_2("DELETE FROM friendships WHERE "
                         "(owner_account = ?1 AND friend_account = ?2) OR "
                         "(owner_account = ?2 AND friend_account = ?1);",
                         account, friend_account);

    if (changed < 0)       result = FRIEND_RESULT_ERROR;
    else if (changed == 0) result = FRIEND_RESULT_NOT_FOUND;
    else                   result = FRIEND_RESULT_OK;

    if (result == FRIEND_RESULT_ERROR) tx_rollback();
    else if (!tx_commit())              result = FRIEND_RESULT_ERROR;

    pthread_mutex_unlock(&g_social_lock);
    return result;
}

FriendResult social_block_add(uint32_t account, uint32_t blocked_account) {
    if (account == blocked_account) return FRIEND_RESULT_SELF;

    pthread_mutex_lock(&g_social_lock);
    if (!g_social_db) {
        pthread_mutex_unlock(&g_social_lock);
        return FRIEND_RESULT_ERROR;
    }

    FriendResult result = FRIEND_RESULT_ERROR;

    /* Bounded, because every online player's block list is mirrored into each
     * world's index so chat can be filtered without a round trip. Checked
     * before the transaction and skipped when the block already exists, so
     * re-blocking somebody at the cap is not an error. */
    if (!block_exists(account, blocked_account)) {
        int held = count_1("SELECT COUNT(*) FROM account_blocks WHERE account_id = ?;",
                           account);
        if (held < 0) {
            pthread_mutex_unlock(&g_social_lock);
            return FRIEND_RESULT_ERROR;
        }
        if (held >= MAX_BLOCKS) {
            pthread_mutex_unlock(&g_social_lock);
            return FRIEND_RESULT_FRIEND_CAP;
        }
    }

    if (!tx_begin()) {
        pthread_mutex_unlock(&g_social_lock);
        return FRIEND_RESULT_ERROR;
    }

    /* One transaction for all three writes. A block that tore down the
     * friendship but failed to record itself would let the very next request
     * straight back through, which is the one outcome a block must never have. */
    int cleared = exec_2("DELETE FROM friendships WHERE "
                         "(owner_account = ?1 AND friend_account = ?2) OR "
                         "(owner_account = ?2 AND friend_account = ?1);",
                         account, blocked_account);

    if (cleared >= 0 && drop_requests_both_ways(account, blocked_account)) {
        const char* sql = "INSERT OR IGNORE INTO account_blocks "
                          "(account_id, blocked_account, created_at) VALUES (?, ?, ?);";
        sqlite3_stmt* stmt = NULL;
        if (sqlite3_prepare_v2(g_social_db, sql, -1, &stmt, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(stmt, 1, (sqlite3_int64)account);
            sqlite3_bind_int64(stmt, 2, (sqlite3_int64)blocked_account);
            sqlite3_bind_int64(stmt, 3, (sqlite3_int64)time(NULL));
            if (sqlite3_step(stmt) == SQLITE_DONE) result = FRIEND_RESULT_OK;
            sqlite3_finalize(stmt);
        }
    }

    if (result == FRIEND_RESULT_ERROR) tx_rollback();
    else if (!tx_commit())              result = FRIEND_RESULT_ERROR;

    pthread_mutex_unlock(&g_social_lock);
    return result;
}

FriendResult social_block_remove(uint32_t account, uint32_t blocked_account) {
    pthread_mutex_lock(&g_social_lock);
    if (!g_social_db) {
        pthread_mutex_unlock(&g_social_lock);
        return FRIEND_RESULT_ERROR;
    }

    int changed = exec_2("DELETE FROM account_blocks "
                         "WHERE account_id = ? AND blocked_account = ?;",
                         account, blocked_account);
    pthread_mutex_unlock(&g_social_lock);

    if (changed < 0) return FRIEND_RESULT_ERROR;
    return changed ? FRIEND_RESULT_OK : FRIEND_RESULT_NOT_FOUND;
}

/* --- Reads --------------------------------------------------------------- */

int social_friend_list(uint32_t account, FriendEntry* out, int max) {
    if (!out || max <= 0) return 0;

    pthread_mutex_lock(&g_social_lock);
    if (!g_social_db) {
        pthread_mutex_unlock(&g_social_lock);
        return -1;
    }

    /* LEFT JOIN, not JOIN: a friend who has never entered a world has no
     * presence row, and an inner join would drop them out of the list entirely
     * rather than showing them as offline. */
    const char* sql =
        "SELECT f.friend_account, "
        "       COALESCE(p.last_world_id, 0), "
        "       COALESCE(p.last_character_name, ''), "
        "       COALESCE(p.last_seen_at, 0) "
        "FROM friendships f "
        "LEFT JOIN account_presence p ON p.account_id = f.friend_account "
        "WHERE f.owner_account = ? "
        "ORDER BY f.created_at DESC, f.friend_account ASC "
        "LIMIT ?;";

    sqlite3_stmt* stmt = NULL;
    if (sqlite3_prepare_v2(g_social_db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        LOG_ERROR("social: friend list prepare failed: %s", sqlite3_errmsg(g_social_db));
        pthread_mutex_unlock(&g_social_lock);
        return -1;
    }
    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)account);
    sqlite3_bind_int(stmt, 2, max);

    int n = 0;
    while (n < max && sqlite3_step(stmt) == SQLITE_ROW) {
        FriendEntry* e = &out[n];
        memset(e, 0, sizeof(*e));

        e->account_id    = (uint32_t)sqlite3_column_int64(stmt, 0);
        e->last_world_id = (uint32_t)sqlite3_column_int64(stmt, 1);

        const char* name = (const char*)sqlite3_column_text(stmt, 2);
        if (name) snprintf(e->last_character_name, sizeof(e->last_character_name), "%s", name);

        e->last_seen_at = (int64_t)sqlite3_column_int64(stmt, 3);
        n++;
    }

    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&g_social_lock);
    return n;
}

int social_friend_requests_incoming(uint32_t account, FriendRequestEntry* out, int max) {
    if (!out || max <= 0) return 0;

    pthread_mutex_lock(&g_social_lock);
    if (!g_social_db) {
        pthread_mutex_unlock(&g_social_lock);
        return -1;
    }

    /* Sweeping here is the only thing that ever expires a request. It runs
     * before the read so the caller cannot be handed a row that is already
     * past its TTL. */
    sweep_stale_requests(account);

    const char* sql =
        "SELECT r.from_account, COALESCE(p.last_character_name, ''), r.created_at "
        "FROM friend_requests r "
        "LEFT JOIN account_presence p ON p.account_id = r.from_account "
        "WHERE r.to_account = ? "
        "ORDER BY r.created_at ASC "
        "LIMIT ?;";

    sqlite3_stmt* stmt = NULL;
    if (sqlite3_prepare_v2(g_social_db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        LOG_ERROR("social: incoming prepare failed: %s", sqlite3_errmsg(g_social_db));
        pthread_mutex_unlock(&g_social_lock);
        return -1;
    }
    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)account);
    sqlite3_bind_int(stmt, 2, max);

    int n = 0;
    while (n < max && sqlite3_step(stmt) == SQLITE_ROW) {
        FriendRequestEntry* e = &out[n];
        memset(e, 0, sizeof(*e));

        e->from_account = (uint32_t)sqlite3_column_int64(stmt, 0);

        const char* name = (const char*)sqlite3_column_text(stmt, 1);
        if (name) snprintf(e->from_name, sizeof(e->from_name), "%s", name);

        e->created_at = (int64_t)sqlite3_column_int64(stmt, 2);
        n++;
    }

    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&g_social_lock);
    return n;
}

int social_friend_requests_outgoing(uint32_t account, uint32_t* out, int max) {
    if (!out || max <= 0) return 0;

    pthread_mutex_lock(&g_social_lock);
    if (!g_social_db) {
        pthread_mutex_unlock(&g_social_lock);
        return -1;
    }

    const char* sql = "SELECT to_account FROM friend_requests "
                      "WHERE from_account = ? ORDER BY created_at ASC LIMIT ?;";
    sqlite3_stmt* stmt = NULL;

    if (sqlite3_prepare_v2(g_social_db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        LOG_ERROR("social: outgoing prepare failed: %s", sqlite3_errmsg(g_social_db));
        pthread_mutex_unlock(&g_social_lock);
        return -1;
    }
    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)account);
    sqlite3_bind_int(stmt, 2, max);

    int n = 0;
    while (n < max && sqlite3_step(stmt) == SQLITE_ROW)
        out[n++] = (uint32_t)sqlite3_column_int64(stmt, 0);

    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&g_social_lock);
    return n;
}

int social_is_friend(uint32_t account, uint32_t other) {
    pthread_mutex_lock(&g_social_lock);
    int found = g_social_db ? friendship_exists(account, other) : 0;
    pthread_mutex_unlock(&g_social_lock);
    return found;
}

int social_is_blocked(uint32_t account, uint32_t other) {
    pthread_mutex_lock(&g_social_lock);
    int found = g_social_db ? block_exists(account, other) : 0;
    pthread_mutex_unlock(&g_social_lock);
    return found;
}

int social_block_list(uint32_t account, uint32_t* out, int max) {
    if (!out || max <= 0) return 0;

    pthread_mutex_lock(&g_social_lock);
    if (!g_social_db) {
        pthread_mutex_unlock(&g_social_lock);
        return -1;
    }

    const char* sql = "SELECT blocked_account FROM account_blocks "
                      "WHERE account_id = ? "
                      "ORDER BY created_at DESC, blocked_account ASC "
                      "LIMIT ?;";

    sqlite3_stmt* stmt = NULL;
    if (sqlite3_prepare_v2(g_social_db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        LOG_ERROR("social: block list prepare failed: %s", sqlite3_errmsg(g_social_db));
        pthread_mutex_unlock(&g_social_lock);
        return -1;
    }
    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)account);
    sqlite3_bind_int(stmt, 2, max);

    int n = 0;
    while (n < max && sqlite3_step(stmt) == SQLITE_ROW)
        out[n++] = (uint32_t)sqlite3_column_int64(stmt, 0);

    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&g_social_lock);
    return n;
}

int social_friend_count(uint32_t account) {
    pthread_mutex_lock(&g_social_lock);
    int n = g_social_db ? friend_count_unlocked(account) : -1;
    pthread_mutex_unlock(&g_social_lock);
    return n;
}

/* --- Presence cache ------------------------------------------------------ */

int social_presence_update(uint32_t account_id, uint32_t world_id,
                           uint32_t character_id, const char* character_name) {
    if (!character_name) character_name = "";

    pthread_mutex_lock(&g_social_lock);
    if (!g_social_db) {
        pthread_mutex_unlock(&g_social_lock);
        return 0;
    }

    /* One row per account, replaced rather than appended. This is a cache of
     * something a world knows, and a second row for the same account would make
     * "where were they last" ambiguous the first time somebody changed world. */
    const char* sql =
        "INSERT INTO account_presence "
        "(account_id, last_world_id, last_character_id, last_character_name, last_seen_at) "
        "VALUES (?, ?, ?, ?, ?) "
        "ON CONFLICT(account_id) DO UPDATE SET "
        "  last_world_id = excluded.last_world_id, "
        "  last_character_id = excluded.last_character_id, "
        "  last_character_name = excluded.last_character_name, "
        "  last_seen_at = excluded.last_seen_at;";

    sqlite3_stmt* stmt = NULL;
    if (sqlite3_prepare_v2(g_social_db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        LOG_ERROR("social: presence prepare failed: %s", sqlite3_errmsg(g_social_db));
        pthread_mutex_unlock(&g_social_lock);
        return 0;
    }
    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)account_id);
    sqlite3_bind_int64(stmt, 2, (sqlite3_int64)world_id);
    sqlite3_bind_int64(stmt, 3, (sqlite3_int64)character_id);
    sqlite3_bind_text(stmt, 4, character_name, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 5, (sqlite3_int64)time(NULL));

    int ok = (sqlite3_step(stmt) == SQLITE_DONE);
    if (!ok) LOG_ERROR("social: presence write failed: %s", sqlite3_errmsg(g_social_db));

    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&g_social_lock);
    return ok;
}

int social_account_by_character_name(const char* character_name,
                                     uint32_t prefer_world_id,
                                     uint32_t* out_account) {
    if (!out_account) return 0;
    *out_account = 0;
    if (!character_name || !*character_name) return 0;

    pthread_mutex_lock(&g_social_lock);
    if (!g_social_db) {
        pthread_mutex_unlock(&g_social_lock);
        return 0;
    }

    /* Ordered so the asking player's own world sorts first, then read at most
     * two rows. Two is all the decision needs: one row means unambiguous, and
     * a second row either shares the preferred world flag with the first --
     * ambiguous -- or does not, in which case the first won on preference.
     *
     * COLLATE NOCASE matches the index, so this stays an index scan.
     */
    const char* sql =
        "SELECT account_id, (last_world_id = ?) AS preferred "
        "FROM account_presence "
        "WHERE last_character_name = ? COLLATE NOCASE "
        "ORDER BY preferred DESC, account_id ASC "
        "LIMIT 2;";

    sqlite3_stmt* stmt = NULL;
    if (sqlite3_prepare_v2(g_social_db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        LOG_ERROR("social: name lookup prepare failed: %s", sqlite3_errmsg(g_social_db));
        pthread_mutex_unlock(&g_social_lock);
        return 0;
    }
    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)prefer_world_id);
    sqlite3_bind_text(stmt, 2, character_name, -1, SQLITE_STATIC);

    uint32_t first_account = 0;
    int first_preferred = 0, rows = 0, second_preferred = 0;

    while (sqlite3_step(stmt) == SQLITE_ROW && rows < 2) {
        uint32_t account   = (uint32_t)sqlite3_column_int64(stmt, 0);
        int      preferred = sqlite3_column_int(stmt, 1);

        if (rows == 0) { first_account = account; first_preferred = preferred; }
        else           { second_preferred = preferred; }
        rows++;
    }
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&g_social_lock);

    if (rows == 0) return 0;

    /* One candidate, or one that won on the asking player's world. Two
     * candidates with no world to separate them is a name that means two
     * different people, and answering it would send a request to a stranger. */
    if (rows == 1 || (first_preferred && !second_preferred)) {
        *out_account = first_account;
        return 1;
    }
    return 0;
}
