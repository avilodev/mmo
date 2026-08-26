/**
 * @file
 * Store login accounts and verify Argon2id password hashes in SQLite.
 */
#include "users_database.h"
#include "log.h"
#include <stdio.h>
#include <string.h>
#include <sqlite3.h>
#include <sodium.h>

static sqlite3* g_db = NULL;
pthread_mutex_t g_db_lock = PTHREAD_MUTEX_INITIALIZER;

/* How long a statement waits for a lock another connection holds before it
 * gives up. Long enough to absorb a concurrent write, short enough that a
 * genuinely stuck database fails a login rather than hanging the thread. */
#define DB_BUSY_TIMEOUT_MS 5000

/**
 * Initialize password hashing, open the account database, and create its user table.
 *
 * @return      Nonzero on success, otherwise zero.
 */
int db_init(const char* db_path) {
    if (sodium_init() < 0) {
        LOG_ERROR("Failed to initialize libsodium");
        return 0;
    }

    int rc = sqlite3_open(db_path, &g_db);

    if (rc != SQLITE_OK) {
        /* sqlite3_open allocates the handle even when it fails, and the error
         * string is read out of it, so it has to be closed rather than
         * abandoned -- and g_db cleared, because every entry point below
         * treats a non-NULL g_db as an open database. */
        LOG_ERROR("Cannot open database: %s", sqlite3_errmsg(g_db));
        sqlite3_close(g_db);
        g_db = NULL;
        return 0;
    }

    /* Write-ahead logging and a busy timeout.
     *
     * The login server verifies credentials on several threads against one
     * handle. In SQLite's default rollback-journal mode a writer locks the
     * whole database against readers, and the default busy handler does not
     * wait at all -- it returns SQLITE_BUSY immediately, which every caller
     * here reports as a failed login. WAL lets readers run alongside the
     * writer, and the timeout turns the remaining writer-vs-writer contention
     * into a short wait instead of a spurious rejection.
     *
     * Neither is fatal if it fails: a database on a filesystem that cannot
     * support WAL still works, just with the old locking. It is logged so the
     * degraded mode is visible rather than assumed. */
    char* pragma_err = NULL;
    if (sqlite3_exec(g_db, "PRAGMA journal_mode=WAL;", NULL, NULL, &pragma_err) != SQLITE_OK) {
        LOG_WARN("Could not enable WAL on %s: %s — falling back to the rollback journal",
                 db_path, pragma_err ? pragma_err : "unknown error");
        sqlite3_free(pragma_err);
        pragma_err = NULL;
    }
    if (sqlite3_busy_timeout(g_db, DB_BUSY_TIMEOUT_MS) != SQLITE_OK)
        LOG_WARN("Could not set a busy timeout on %s", db_path);

    // Create users table with email and birthday fields
    const char* create_table =
        "CREATE TABLE IF NOT EXISTS users ("
        "player_id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "username TEXT UNIQUE NOT NULL,"
        "password TEXT NOT NULL,"
        "email TEXT NOT NULL,"
        "birthday TEXT NOT NULL,"
        "created_at DATETIME DEFAULT CURRENT_TIMESTAMP"
        ");";

    char* err_msg = NULL;
    rc = sqlite3_exec(g_db, create_table, NULL, NULL, &err_msg);

    if (rc != SQLITE_OK) {
        LOG_ERROR("Cannot create the users table in %s: %s", db_path, err_msg ? err_msg : "unknown error");
        sqlite3_free(err_msg);
        sqlite3_close(g_db);
        g_db = NULL;
        return 0;
    }

    LOG_INFO("Database initialized: %s", db_path);
    return 1;
}

/** Close the account database when open. */
void db_close(void) {
    if (g_db) {
        sqlite3_close(g_db);
        g_db = NULL;
    }
}

/**
 * Hash a password and insert a new account under the database mutex.
 *
 * @return      The generated player identifier, or zero when hashing or insertion fails.
 */
uint32_t db_create_user(const char* username, const char* password, const char* email, const char* birthday) {
    if (!g_db) {
        LOG_ERROR("db_create_user: the account database is not open");
        return 0;
    }

    // Hash the password with Argon2id before storing
    char password_hash[crypto_pwhash_STRBYTES];
    if (crypto_pwhash_str(password_hash, password, strlen(password),
                          crypto_pwhash_OPSLIMIT_INTERACTIVE,
                          crypto_pwhash_MEMLIMIT_INTERACTIVE) != 0) {
        LOG_ERROR("db_create_user: out of memory during password hash");
        return 0;
    }

    pthread_mutex_lock(&g_db_lock);

    LOG_DEBUG("Attempting to create user: '%s'", username);

    const char* sql = "INSERT INTO users (username, password, email, birthday) VALUES (?, ?, ?, ?);";
    sqlite3_stmt* stmt;

    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        LOG_ERROR("Failed to prepare statement: %s", sqlite3_errmsg(g_db));
        pthread_mutex_unlock(&g_db_lock);
        return 0;
    }

    sqlite3_bind_text(stmt, 1, username,      -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, password_hash, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, email,         -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4, birthday,      -1, SQLITE_STATIC);

    int rc = sqlite3_step(stmt);

    if (rc != SQLITE_DONE) {
        LOG_ERROR("Failed to insert user: %s (error code: %d)", sqlite3_errmsg(g_db), rc);
        sqlite3_finalize(stmt);
        pthread_mutex_unlock(&g_db_lock);
        return 0;
    }

    uint32_t player_id = (uint32_t)sqlite3_last_insert_rowid(g_db);
    sqlite3_finalize(stmt);

    LOG_INFO("Created account with player_id: %u", player_id);
    LOG_DEBUG("player_id %u is username '%s'", player_id, username);

    pthread_mutex_unlock(&g_db_lock);
    return player_id;
}

/**
 * Verify supplied credentials against the stored password hash.
 *
 * The database lock is held only for the lookup, and released before the hash
 * is checked. That split is the whole performance characteristic of logging in.
 *
 * Argon2id is deliberately expensive -- around 85ms here, which is the point of
 * it -- and it needs nothing but the string it was handed. Verifying inside the
 * lock therefore serialised every login on the server behind one hash, pinning
 * the whole service at roughly twelve logins a second no matter how many cores,
 * event loops or workers were pointed at it: measured, eight concurrent clients
 * produced eight times the latency and exactly the same throughput. Copying the
 * hash out and verifying without the lock lets those logins actually run at
 * once.
 *
 * @return      The matching player identifier, or zero when credentials cannot
 *              be verified.
 */
/** Storage for the hash the username-miss path verifies against. */
static char           g_decoy_hash[crypto_pwhash_STRBYTES];
static pthread_once_t g_decoy_once = PTHREAD_ONCE_INIT;

/** Hash random input at the parameters registration uses.
 *
 * The value is never compared for equality with anything and never stored; it
 * exists so a verify against it costs what a real verify costs.
 */
static void build_decoy_hash(void) {
    unsigned char noise[32];
    randombytes_buf(noise, sizeof(noise));

    if (crypto_pwhash_str(g_decoy_hash, (const char*)noise, sizeof(noise),
                          crypto_pwhash_OPSLIMIT_INTERACTIVE,
                          crypto_pwhash_MEMLIMIT_INTERACTIVE) == 0) return;

    /* Hashing failed. Fall back to a fixed Argon2id string at the same
     * parameters, which verifies -- and therefore costs -- the same. */
    snprintf(g_decoy_hash, sizeof(g_decoy_hash), "%s",
             "$argon2id$v=19$m=65536,t=2,p=1$c29tZXNhbHRzb21lc2FsdA$"
             "RdescudvJCsgt3ub+b+dWRWJTmaaJObGXtSPZTFa/Hg");
}

/** Return a hash to verify against when no account matched.
 *
 * Built once, from random input, at the same cost parameters registration
 * uses. It exists to be verified against and never to match: the point is the
 * time it takes, so the parameters must be the ones registration writes.
 *
 * If hashing at startup fails, a known-good Argon2id string at the same
 * parameters is copied in instead -- a miss path that skips the work is
 * precisely the defect this closes, so there is no "give up" branch. The
 * literal is copied into the full-size buffer rather than returned directly,
 * because crypto_pwhash_str_verify() takes a char[crypto_pwhash_STRBYTES] and
 * may read the whole array.
 */
static const char* decoy_hash(void) {
    pthread_once(&g_decoy_once, build_decoy_hash);
    return g_decoy_hash;
}

uint32_t db_verify_user(const char* username, const char* password) {
    if (!g_db) return 0;

    /* Copied out rather than pointed at: the text sqlite3_column_text() returns
     * belongs to the statement and dies with the finalize below. */
    char     stored_hash[crypto_pwhash_STRBYTES];
    uint32_t candidate_id = 0;
    int      found = 0;

    pthread_mutex_lock(&g_db_lock);

    // Fetch the stored hash by username only — never compare password in SQL
    const char* sql = "SELECT player_id, password FROM users WHERE username = ?;";
    sqlite3_stmt* stmt;

    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        LOG_ERROR("Failed to prepare statement: %s", sqlite3_errmsg(g_db));
        pthread_mutex_unlock(&g_db_lock);
        return 0;
    }

    sqlite3_bind_text(stmt, 1, username, -1, SQLITE_STATIC);

    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* hash = (const char*)sqlite3_column_text(stmt, 1);
        if (hash && strlen(hash) < sizeof(stored_hash)) {
            snprintf(stored_hash, sizeof(stored_hash), "%s", hash);
            candidate_id = (uint32_t)sqlite3_column_int(stmt, 0);
            found = 1;
        }
    }

    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&g_db_lock);

    if (!found) {
        /* Verify against a decoy hash rather than returning immediately.
         *
         * An Argon2id verify at these parameters costs on the order of 85ms.
         * Returning early on an unknown username answered those in microseconds
         * and known ones in 85 milliseconds, which is a username oracle
         * anybody can read off a stopwatch: an attacker enumerates the whole
         * account list before trying a single password. Spending the same work
         * on a hash nobody's password matches makes the two paths cost the
         * same. */
        int ignored = crypto_pwhash_str_verify(decoy_hash(), password, strlen(password));
        (void)ignored;   /* the work is the point; the answer is never used */
        return 0;
    }

    /* Outside the lock. Nothing here touches the database. */
    return crypto_pwhash_str_verify(stored_hash, password, strlen(password)) == 0
         ? candidate_id : 0;
}
