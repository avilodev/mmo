/**
 * @file
 * Manage per-world PostgreSQL connection pools and realm character operations.
 */
#include "world_database_manager.h"
#include "world_database_config.h"

#include "types.h"
#include "players_database.h"

#include <libpq-fe.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>

static WorldDatabasePool g_world_pools[MAX_WORLDS + 1] = {0}; // index 0 unused, 1-10 for worlds
static pthread_mutex_t g_manager_lock = PTHREAD_MUTEX_INITIALIZER;

/**
 * Initialize a world's fixed-size PostgreSQL connection pool.
 *
 * @return      Nonzero when the pool is ready, otherwise zero.
 */
static int init_world_pool(uint32_t world_id) {
    if (world_id < 1 || world_id > MAX_WORLDS) {
        return 0;
    }

    WorldDatabasePool* pool = &g_world_pools[world_id];

    pthread_mutex_lock(&g_manager_lock);

    if (pool->initialized) {
        pthread_mutex_unlock(&g_manager_lock);
        return 1; // Already initialized
    }

    const char* conn_str = get_database_for_world_id(world_id);
    if (!conn_str) {
        pthread_mutex_unlock(&g_manager_lock);
        return 0;
    }

    pool->world_id = world_id;
    snprintf(pool->db_name, sizeof(pool->db_name), "%s", get_world_name_by_id(world_id));
    pthread_mutex_init(&pool->pool_lock, NULL);
    pthread_cond_init(&pool->conn_available, NULL);

    // Create connections
    for (int i = 0; i < CONN_PER_WORLD; i++) {
        pthread_mutex_init(&pool->connections[i].lock, NULL);
        pool->connections[i].in_use = 0;
        pool->connections[i].conn = PQconnectdb(conn_str);

        if (PQstatus(pool->connections[i].conn) != CONNECTION_OK) {
            fprintf(stderr, "Failed to connect to %s database: %s\n",
                    pool->db_name, PQerrorMessage(pool->connections[i].conn));

            // Cleanup
            for (int j = 0; j < i; j++) {
                if (pool->connections[j].conn) {
                    PQfinish(pool->connections[j].conn);
                }
                pthread_mutex_destroy(&pool->connections[j].lock);
            }
            pthread_mutex_unlock(&g_manager_lock);
            return 0;
        }
    }

    pool->initialized = 1;
    printf("Initialized database pool for world '%s' (ID: %u)\n", pool->db_name, world_id);

    pthread_mutex_unlock(&g_manager_lock);
    return 1;
}

/**
 * Acquire a connection from a world's pool, initializing the pool when needed.
 *
 * This function may block for up to 30 seconds waiting for a connection; the caller must release a returned connection.
 *
 * @return      An acquired connection, or NULL for an invalid world, initialization failure, or timeout.
 */
static PGconn* acquire_world_connection(uint32_t world_id) {
    if (world_id < 1 || world_id > MAX_WORLDS) {
        return NULL;
    }

    // Initialize pool if needed
    if (!g_world_pools[world_id].initialized) {
        if (!init_world_pool(world_id)) {
            return NULL;
        }
    }

    WorldDatabasePool* pool = &g_world_pools[world_id];

    struct timespec timeout;
    clock_gettime(CLOCK_REALTIME, &timeout);
    timeout.tv_sec += 30;  // 30 second timeout

    pthread_mutex_lock(&pool->pool_lock);

    while (1) {
        // Find available connection
        for (int i = 0; i < CONN_PER_WORLD; i++) {
            pthread_mutex_lock(&pool->connections[i].lock);
            if (!pool->connections[i].in_use) {
                pool->connections[i].in_use = 1;
                pthread_mutex_unlock(&pool->connections[i].lock);
                pthread_mutex_unlock(&pool->pool_lock);
                return pool->connections[i].conn;
            }
            pthread_mutex_unlock(&pool->connections[i].lock);
        }

        // No connections available - wait with timeout
        int wait_result = pthread_cond_timedwait(&pool->conn_available,
                                                  &pool->pool_lock, &timeout);

        if (wait_result == ETIMEDOUT) {
            pthread_mutex_unlock(&pool->pool_lock);
            fprintf(stderr, "Timeout waiting for connection to world %u\n", world_id);
            return NULL;
        }
    }
}

/** Return an acquired connection to its world pool and wake one waiter. */
static void release_world_connection(uint32_t world_id, PGconn* conn) {
    if (world_id < 1 || world_id > MAX_WORLDS || !conn) {
        return;
    }

    WorldDatabasePool* pool = &g_world_pools[world_id];
    pthread_mutex_lock(&pool->pool_lock);

    for (int i = 0; i < CONN_PER_WORLD; i++) {
        if (pool->connections[i].conn == conn) {
            pthread_mutex_lock(&pool->connections[i].lock);
            pool->connections[i].in_use = 0;
            pthread_mutex_unlock(&pool->connections[i].lock);
            pthread_cond_signal(&pool->conn_available);
            break;
        }
    }

    pthread_mutex_unlock(&pool->pool_lock);
}

/**
 * Load an account's characters from a world's database.
 *
 * @return      The number of records written, or zero when the query fails or the output is invalid.
 */
int world_character_get_list(uint32_t account_id, uint32_t world_id,
                             CharacterInfo* characters, int max_count) {
    if (!characters || max_count <= 0) {
        return 0;
    }

    PGconn* conn = acquire_world_connection(world_id);
    if (!conn) {
        return 0;
    }

    char account_id_str[32];
    char world_id_str[32];
    snprintf(account_id_str, sizeof(account_id_str), "%u", account_id);
    snprintf(world_id_str, sizeof(world_id_str), "%u", world_id);

    const char* param_values[2] = {account_id_str, world_id_str};

    // Ordering is by character_id, not created_at DESC.
    //
    // created_at DESC put each newly created character FIRST, renumbering every
    // existing row, so a client index captured before a create pointed at a
    // different character afterwards. character_id is the serial primary key:
    // unique, monotonic with creation, and already indexed, so ordering by it
    // is stable and append-only -- existing positions never move.
    //
    // world_id is now actually applied. It was accepted as a parameter, built
    // into param_values, and then never used: the query passed a parameter
    // count of 1 and filtered on account_id alone. That is currently masked
    // because each world has its own database, but it made the world_id
    // argument a lie and would silently mix worlds the moment two share one.
    PGresult* res = PQexecParams(conn,
        "SELECT character_id, name, level, class_id, race_id, "
        "       pos_x, pos_y, pos_z, health, max_health "
        "FROM characters "
        "WHERE account_id = $1 AND world_id = $2 "
        "ORDER BY character_id ASC "
        "LIMIT 10",
        2, NULL, param_values, NULL, NULL, 0
    );

    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        fprintf(stderr, "Failed to get character list for world %u: %s\n",
                world_id, PQerrorMessage(conn));
        PQclear(res);
        release_world_connection(world_id, conn);
        return 0;
    }

    int count = PQntuples(res);
    if (count > max_count) {
        count = max_count;
    }

    for (int i = 0; i < count; i++) {
        characters[i].character_id = (uint32_t)strtoul(PQgetvalue(res, i, 0), NULL, 10);
        strncpy(characters[i].name, PQgetvalue(res, i, 1), 31);
        characters[i].name[31] = '\0';
        characters[i].level = (uint32_t)strtoul(PQgetvalue(res, i, 2), NULL, 10);
        /* class_id and race_id hold the same fused identifier; class_id is authoritative. */
        characters[i].race_id = (uint32_t)strtoul(PQgetvalue(res, i, 3), NULL, 10);
        characters[i].pos_x = (float)strtod(PQgetvalue(res, i, 5), NULL);
        characters[i].pos_y = (float)strtod(PQgetvalue(res, i, 6), NULL);
        characters[i].health = (uint32_t)strtoul(PQgetvalue(res, i, 8), NULL, 10);
        characters[i].max_health = (uint32_t)strtoul(PQgetvalue(res, i, 9), NULL, 10);
    }

    PQclear(res);
    release_world_connection(world_id, conn);

    printf("Retrieved %d characters for account %u from world '%s'\n",
           count, account_id, get_world_name_by_id(world_id));

    return count;
}

/**
 * Count an account's characters in a world database.
 *
 * @return      The character count, or -1 when a connection or query fails.
 */
int world_character_count(uint32_t account_id, uint32_t world_id) {
    PGconn* conn = acquire_world_connection(world_id);
    if (!conn) return -1;

    char account_id_str[32];
    char world_id_str[32];
    snprintf(account_id_str, sizeof(account_id_str), "%u", account_id);
    snprintf(world_id_str, sizeof(world_id_str), "%u", world_id);
    const char* params[2] = {account_id_str, world_id_str};

    // Scoped to the world whose per-world limit this count enforces; it
    // previously counted on account_id alone.
    PGresult* res = PQexecParams(conn,
        "SELECT COUNT(*) FROM characters WHERE account_id = $1 AND world_id = $2",
        2, NULL, params, NULL, NULL, 0);

    int count = -1;
    if (PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) == 1) {
        count = atoi(PQgetvalue(res, 0, 0));
    } else {
        fprintf(stderr, "Failed to count characters for world %u: %s\n",
                world_id, PQerrorMessage(conn));
    }

    PQclear(res);
    release_world_connection(world_id, conn);
    return count;
}

/**
 * Insert a character within a PostgreSQL transaction.
 *
 * @param out_character_id  Receives the generated identifier on success.
 * @return                  Nonzero on success, otherwise zero.
 */
int world_character_create(uint32_t account_id, uint32_t world_id,
                           const char* name, int class_id, int race_id,
                           uint32_t* out_character_id) {
    if (!name || !out_character_id) {
        return 0;
    }

    PGconn* conn = acquire_world_connection(world_id);
    if (!conn) {
        return 0;
    }

    // Begin transaction
    PGresult* res = PQexec(conn, "BEGIN");
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        PQclear(res);
        release_world_connection(world_id, conn);
        return 0;
    }
    PQclear(res);

    char account_id_str[32];
    char world_id_str[32];
    char class_id_str[32];
    char race_id_str[32];

    snprintf(account_id_str, sizeof(account_id_str), "%u", account_id);
    snprintf(world_id_str, sizeof(world_id_str), "%u", world_id);
    snprintf(class_id_str, sizeof(class_id_str), "%d", class_id);
    snprintf(race_id_str, sizeof(race_id_str), "%d", race_id);

    const char* param_values[5] = {account_id_str, world_id_str, name, class_id_str, race_id_str};

    res = PQexecParams(conn,
        "INSERT INTO characters (account_id, world_id, name, class_id, race_id) "
        "VALUES ($1, $2, $3, $4, $5) "
        "RETURNING character_id",
        5, NULL, param_values, NULL, NULL, 0
    );

    if (PQresultStatus(res) != PGRES_TUPLES_OK || PQntuples(res) == 0) {
        fprintf(stderr, "Failed to create character in world %u: %s\n",
                world_id, PQerrorMessage(conn));
        PQclear(res);
        PQexec(conn, "ROLLBACK");
        release_world_connection(world_id, conn);
        return 0;
    }

    *out_character_id = (uint32_t)strtoul(PQgetvalue(res, 0, 0), NULL, 10);
    PQclear(res);

    // Commit. An unchecked COMMIT meant a failed transaction still returned
    // success: the client was told the character existed, then the very next
    // list query correctly showed it missing.
    res = PQexec(conn, "COMMIT");
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        fprintf(stderr, "Failed to commit character '%s' in world %u: %s\n",
                name, world_id, PQerrorMessage(conn));
        PQclear(res);
        PQexec(conn, "ROLLBACK");
        release_world_connection(world_id, conn);
        return 0;
    }
    PQclear(res);

    release_world_connection(world_id, conn);

    printf("Created character '%s' (ID: %u) in world '%s'\n",
           name, *out_character_id, get_world_name_by_id(world_id));

    return 1;
}

/**
 * Test whether a character belongs to an account in a world database.
 *
 * @return      Nonzero when an ownership row exists, otherwise zero.
 */
int world_character_belongs_to_account(uint32_t character_id, uint32_t account_id, uint32_t world_id) {
    PGconn* conn = acquire_world_connection(world_id);
    if (!conn) {
        return 0;
    }

    char character_id_str[32];
    char account_id_str[32];
    snprintf(character_id_str, sizeof(character_id_str), "%u", character_id);
    snprintf(account_id_str, sizeof(account_id_str), "%u", account_id);

    const char* param_values[2] = {character_id_str, account_id_str};

    PGresult* res = PQexecParams(conn,
        "SELECT 1 FROM characters WHERE character_id = $1 AND account_id = $2",
        2, NULL, param_values, NULL, NULL, 0
    );

    int belongs = (PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) > 0);
    PQclear(res);
    release_world_connection(world_id, conn);

    return belongs;
}

/**
 * Delete an account-owned character within a PostgreSQL transaction.
 *
 * @return      Nonzero when a row is deleted, otherwise zero.
 */
int world_character_delete(uint32_t account_id, uint32_t character_id,
                           uint32_t world_id) {
    PGconn* conn = acquire_world_connection(world_id);
    if (!conn) {
        return 0;
    }

    PGresult* res = PQexec(conn, "BEGIN");
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        PQclear(res);
        release_world_connection(world_id, conn);
        return 0;
    }
    PQclear(res);

    char account_id_str[32];
    char character_id_str[32];

    snprintf(account_id_str, sizeof(account_id_str), "%u", account_id);
    snprintf(character_id_str, sizeof(character_id_str), "%u", character_id);

    const char* param_values[2] = {character_id_str, account_id_str};

    res = PQexecParams(conn,
        "DELETE FROM characters WHERE character_id = $1 AND account_id = $2",
        2, NULL, param_values, NULL, NULL, 0
    );

    int success = (PQresultStatus(res) == PGRES_COMMAND_OK &&
                   atoi(PQcmdTuples(res)) > 0);
    PQclear(res);

    res = PQexec(conn, "COMMIT");
    PQclear(res);

    release_world_connection(world_id, conn);

    if (success) {
        printf("Deleted character %u from world '%s'\n",
               character_id, get_world_name_by_id(world_id));
    }

    return success;
}

/** Close all initialized world pools and destroy their synchronization objects. */
void world_databases_cleanup(void) {
    for (int w = 1; w <= MAX_WORLDS; w++) {
        WorldDatabasePool* pool = &g_world_pools[w];
        if (pool->initialized) {
            for (int i = 0; i < CONN_PER_WORLD; i++) {
                if (pool->connections[i].conn) {
                    PQfinish(pool->connections[i].conn);
                }
                pthread_mutex_destroy(&pool->connections[i].lock);
            }
            pthread_mutex_destroy(&pool->pool_lock);
        }
    }
    printf("Cleaned up all world database connections\n");
}
