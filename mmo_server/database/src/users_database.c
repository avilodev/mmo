#include "users_database.h"
#include <stdio.h>
#include <string.h>
#include <sqlite3.h>
#include <sodium.h>

static sqlite3* g_db = NULL;
pthread_mutex_t g_db_lock = PTHREAD_MUTEX_INITIALIZER;

int db_init(const char* db_path) {
    if (sodium_init() < 0) {
        printf("Failed to initialize libsodium\n");
        return 0;
    }

    int rc = sqlite3_open(db_path, &g_db);

    if (rc != SQLITE_OK) {
        printf("Cannot open database: %s\n", sqlite3_errmsg(g_db));
        return 0;
    }

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
        printf("SQL error: %s\n", err_msg);
        sqlite3_free(err_msg);
        return 0;
    }

    printf("Database initialized: %s\n", db_path);
    return 1;
}

void db_close(void) {
    if (g_db) {
        sqlite3_close(g_db);
        g_db = NULL;
    }
}

uint32_t db_create_user(const char* username, const char* password, const char* email, const char* birthday) {
    if (!g_db) {
        printf("db_create_user: g_db is NULL\n");
        return 0;
    }

    // Hash the password with Argon2id before storing
    char password_hash[crypto_pwhash_STRBYTES];
    if (crypto_pwhash_str(password_hash, password, strlen(password),
                          crypto_pwhash_OPSLIMIT_INTERACTIVE,
                          crypto_pwhash_MEMLIMIT_INTERACTIVE) != 0) {
        printf("db_create_user: out of memory during password hash\n");
        return 0;
    }

    pthread_mutex_lock(&g_db_lock);

    printf("Attempting to create user: '%s'\n", username);

    const char* sql = "INSERT INTO users (username, password, email, birthday) VALUES (?, ?, ?, ?);";
    sqlite3_stmt* stmt;

    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        printf("Failed to prepare statement: %s\n", sqlite3_errmsg(g_db));
        pthread_mutex_unlock(&g_db_lock);
        return 0;
    }

    sqlite3_bind_text(stmt, 1, username,      -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, password_hash, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, email,         -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4, birthday,      -1, SQLITE_STATIC);

    int rc = sqlite3_step(stmt);

    if (rc != SQLITE_DONE) {
        printf("Failed to insert user: %s (error code: %d)\n", sqlite3_errmsg(g_db), rc);
        sqlite3_finalize(stmt);
        pthread_mutex_unlock(&g_db_lock);
        return 0;
    }

    uint32_t player_id = (uint32_t)sqlite3_last_insert_rowid(g_db);
    sqlite3_finalize(stmt);

    printf("Created user '%s' with player_id: %u\n", username, player_id);

    pthread_mutex_unlock(&g_db_lock);
    return player_id;
}

uint32_t db_verify_user(const char* username, const char* password) {
    if (!g_db) return 0;

    pthread_mutex_lock(&g_db_lock);

    // Fetch the stored hash by username only — never compare password in SQL
    const char* sql = "SELECT player_id, password FROM users WHERE username = ?;";
    sqlite3_stmt* stmt;

    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        printf("Failed to prepare statement: %s\n", sqlite3_errmsg(g_db));
        pthread_mutex_unlock(&g_db_lock);
        return 0;
    }

    sqlite3_bind_text(stmt, 1, username, -1, SQLITE_STATIC);

    uint32_t player_id = 0;

    if (sqlite3_step(stmt) == SQLITE_ROW) {
        uint32_t db_player_id = (uint32_t)sqlite3_column_int(stmt, 0);
        const char* stored_hash = (const char*)sqlite3_column_text(stmt, 1);

        // Verify the provided password against the stored Argon2id hash
        if (stored_hash &&
            crypto_pwhash_str_verify(stored_hash, password, strlen(password)) == 0) {
            player_id = db_player_id;
        }
    }

    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&g_db_lock);
    return player_id;
}
