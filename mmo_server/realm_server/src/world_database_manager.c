/**
 * @file
 * Manage per-world PostgreSQL connection pools and realm character operations.
 */
#include "world_database_manager.h"
#include "log.h"
#include "world_database_config.h"
#include "world_table.h"

#include "types.h"
#include "players_database.h"

#include <libpq-fe.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>

/** One pool per configured world, indexed by `world_id - 1`.
 *
 * Sized from the world roster at startup rather than from a compiled maximum,
 * so adding a world to worlds.conf needs no change here and no recompile.
 */
static WorldDatabasePool* g_world_pools = NULL;
static size_t             g_pool_count  = 0;

/** Connections opened per world. Set once, from the environment or the default. */
static int g_conn_per_world = CONN_PER_WORLD_DEFAULT;

static pthread_once_t g_pools_once  = PTHREAD_ONCE_INIT;
static int            g_pools_ready = 0;

/** Read the per-world connection count from the environment.
 *
 * @return The configured count, or CONN_PER_WORLD_DEFAULT when unset or unusable.
 */
static int configured_conn_per_world(void) {
    const char* value = getenv("MMO_WORLD_POOL_SIZE");
    if (!value || !*value) return CONN_PER_WORLD_DEFAULT;

    char* end = NULL;
    long parsed = strtol(value, &end, 10);
    if (end == value || *end || parsed < 1 || parsed > 1024) {
        LOG_ERROR("MMO_WORLD_POOL_SIZE='%s' is not a usable pool size; "
                          "using %d", value, CONN_PER_WORLD_DEFAULT);
        return CONN_PER_WORLD_DEFAULT;
    }
    return (int)parsed;
}

/** Report the per-world connection count this process will open.
 *
 * Exposed because the realm's blocking worker pool has to be sized from it:
 * every realm packet is a query against one of these pools, and running more
 * workers than there are connections turns a bounded wait in the reactor's
 * queue into a five-second acquire timeout and a failed query. Safe to call
 * before the pools are opened -- it reads the same environment they do.
 */
int world_database_manager_conn_per_world(void) {
    return configured_conn_per_world();
}

/**
 * Allocate one pool per configured world and create its synchronization objects.
 *
 * Runs exactly once for the process. Split out of init_world_pool() because
 * that function runs again after a failed connect. It used to re-run
 * pthread_mutex_init() and pthread_cond_init() on objects that already
 * existed, which is undefined behaviour -- and on a PostgreSQL outage it
 * happened on every retry, that is, on every character screen the realm served
 * until the database came back.
 */
static void pools_init_once(void) {
    size_t worlds = world_table_count();
    if (worlds == 0) {
        LOG_ERROR("No worlds configured; the realm has no world databases "
                          "to open. Check worlds.conf.");
        return;
    }

    g_conn_per_world = configured_conn_per_world();

    g_world_pools = calloc(worlds, sizeof(*g_world_pools));
    if (!g_world_pools) {
        LOG_ERROR("Out of memory allocating %zu world database pools", worlds);
        return;
    }

    for (size_t w = 0; w < worlds; w++) {
        WorldDatabasePool* pool = &g_world_pools[w];
        pool->connections = calloc((size_t)g_conn_per_world, sizeof(*pool->connections));
        if (!pool->connections) {
            LOG_ERROR("Out of memory allocating world pool %zu", w + 1);
            /* Release what this call already built; the ready flag stays clear,
             * so every acquire fails cleanly rather than touching half a table. */
            for (size_t done = 0; done < w; done++) {
                for (int i = 0; i < g_world_pools[done].conn_count; i++)
                    pthread_mutex_destroy(&g_world_pools[done].connections[i].lock);
                pthread_cond_destroy(&g_world_pools[done].conn_available);
                pthread_mutex_destroy(&g_world_pools[done].pool_lock);
                pthread_mutex_destroy(&g_world_pools[done].init_lock);
                free(g_world_pools[done].connections);
            }
            free(g_world_pools);
            g_world_pools = NULL;
            return;
        }
        pool->conn_count = g_conn_per_world;
        pool->world_id   = (uint32_t)w + 1;
        pthread_mutex_init(&pool->init_lock, NULL);
        pthread_mutex_init(&pool->pool_lock, NULL);
        pthread_cond_init(&pool->conn_available, NULL);
        for (int i = 0; i < pool->conn_count; i++)
            pthread_mutex_init(&pool->connections[i].lock, NULL);
    }

    g_pool_count  = worlds;
    g_pools_ready = 1;
}

/** Resolve a world's pool, sizing the pool table on first use.
 *
 * @return The pool, or NULL when the identifier names no configured world.
 */
static WorldDatabasePool* pool_for(uint32_t world_id) {
    pthread_once(&g_pools_once, pools_init_once);
    if (!g_pools_ready) return NULL;
    if (world_id < 1 || (size_t)world_id > g_pool_count) return NULL;
    return &g_world_pools[world_id - 1];
}

/** Number of world pools the realm allocated, one per configured world. */
size_t world_database_pool_count(void) {
    pthread_once(&g_pools_once, pools_init_once);
    return g_pool_count;
}

/**
 * Open a world's PostgreSQL connection pool.
 *
 * Holds only this world's init_lock, so opening one world's connections does not
 * delay any other world's.
 *
 * @return      Nonzero when the pool is ready, otherwise zero.
 */
static int init_world_pool(uint32_t world_id) {
    WorldDatabasePool* pool = pool_for(world_id);
    if (!pool) return 0;

    pthread_mutex_lock(&pool->init_lock);

    // Re-checked under the lock: two threads can both miss the fast path.
    if (atomic_load_explicit(&pool->initialized, memory_order_acquire)) {
        pthread_mutex_unlock(&pool->init_lock);
        return 1;
    }

    const char* conn_str = get_database_for_world_id(world_id);
    if (!conn_str) {
        pthread_mutex_unlock(&pool->init_lock);
        return 0;
    }

    snprintf(pool->db_name, sizeof(pool->db_name), "%s", get_world_name_by_id(world_id));

    // Create connections
    for (int i = 0; i < pool->conn_count; i++) {
        pool->connections[i].in_use = 0;
        pool->connections[i].conn = PQconnectdb(conn_str);

        if (PQstatus(pool->connections[i].conn) != CONNECTION_OK) {
            LOG_ERROR("Failed to connect to %s database: %s",
                      pool->db_name, PQerrorMessage(pool->connections[i].conn));

            /* Close every handle opened by this attempt, this one included.
             * PQconnectdb returns an object on failure too, and the failed one
             * used to be left behind — four leaked handles per retry, forever,
             * while the database was down. */
            for (int j = 0; j <= i; j++) {
                PQfinish(pool->connections[j].conn);
                pool->connections[j].conn = NULL;
            }
            pthread_mutex_unlock(&pool->init_lock);
            return 0;
        }
    }

    // Release store: a thread that sees this flag also sees the connections.
    atomic_store_explicit(&pool->initialized, 1, memory_order_release);
    LOG_INFO("Initialized database pool for world '%s' (ID: %u, %d connections)",
             pool->db_name, world_id, pool->conn_count);

    pthread_mutex_unlock(&pool->init_lock);
    return 1;
}

/**
 * Reset one world connection whose status has gone bad.
 *
 * Without this a single PostgreSQL restart left every realm world pool
 * permanently broken: the handles stayed checked in and were handed out
 * forever in CONNECTION_BAD, so every character list, create and delete failed
 * until the realm process itself was restarted. players_database.c has had the
 * equivalent for its own pool; this is the same treatment for the realm's.
 *
 * @return 1 when the connection is usable, otherwise 0.
 */
static int world_reconnect_if_needed(WorldDatabasePool* pool, WorldConnection* wc) {
    if (wc->conn && PQstatus(wc->conn) == CONNECTION_OK) return 1;

    if (!wc->conn) {
        /* Lost during a previous failed reset. Open it from scratch rather
         * than leaving the slot permanently dead. */
        const char* conn_str = get_database_for_world_id(pool->world_id);
        if (!conn_str) return 0;
        wc->conn = PQconnectdb(conn_str);
        if (PQstatus(wc->conn) == CONNECTION_OK) {
            LOG_INFO("Reopened a connection to world '%s'", pool->db_name);
            return 1;
        }
        LOG_ERROR("Failed to reopen a connection to world '%s': %s",
                  pool->db_name, PQerrorMessage(wc->conn));
        PQfinish(wc->conn);
        wc->conn = NULL;
        return 0;
    }

    LOG_ERROR("Connection to world '%s' lost, attempting to reconnect...",
              pool->db_name);
    PQreset(wc->conn);

    if (PQstatus(wc->conn) == CONNECTION_OK) {
        LOG_INFO("Reconnected to world '%s'", pool->db_name);
        return 1;
    }

    LOG_ERROR("Reconnection to world '%s' failed: %s",
              pool->db_name, PQerrorMessage(wc->conn));
    return 0;
}

/**
 * Acquire a healthy connection from a world's pool, initializing the pool when needed.
 *
 * This function may block for up to 30 seconds waiting for a connection; the caller must release a returned connection.
 *
 * @return      An acquired connection, or NULL for an invalid world, initialization failure, or timeout.
 */
static PGconn* acquire_world_connection(uint32_t world_id) {
    WorldDatabasePool* pool = pool_for(world_id);
    if (!pool) return NULL;

    // Initialize pool if needed. The load is atomic because init_world_pool()
    // publishes the flag from another thread; a plain read here was a data race.
    if (!atomic_load_explicit(&pool->initialized, memory_order_acquire)) {
        if (!init_world_pool(world_id)) {
            return NULL;
        }
    }

    struct timespec timeout;
    clock_gettime(CLOCK_REALTIME, &timeout);
    timeout.tv_sec += 30;  // 30 second timeout

    pthread_mutex_lock(&pool->pool_lock);

    while (1) {
        // Find available connection
        for (int i = 0; i < pool->conn_count; i++) {
            pthread_mutex_lock(&pool->connections[i].lock);
            if (!pool->connections[i].in_use) {
                pool->connections[i].in_use = 1;

                /* Health-check before handing it out. A slot that cannot be
                 * revived is released and skipped rather than returned dead,
                 * and the waiter is signalled so it is not left asleep on a
                 * connection that is in fact free. */
                if (!world_reconnect_if_needed(pool, &pool->connections[i])) {
                    pool->connections[i].in_use = 0;
                    pthread_mutex_unlock(&pool->connections[i].lock);
                    pthread_cond_signal(&pool->conn_available);
                    continue;
                }

                PGconn* conn = pool->connections[i].conn;
                pthread_mutex_unlock(&pool->connections[i].lock);
                pthread_mutex_unlock(&pool->pool_lock);
                return conn;
            }
            pthread_mutex_unlock(&pool->connections[i].lock);
        }

        // No connections available - wait with timeout
        int wait_result = pthread_cond_timedwait(&pool->conn_available,
                                                  &pool->pool_lock, &timeout);

        if (wait_result == ETIMEDOUT) {
            pthread_mutex_unlock(&pool->pool_lock);
            LOG_ERROR("Timeout waiting for connection to world %u", world_id);
            return NULL;
        }
    }
}

/** Return an acquired connection to its world pool and wake one waiter. */
static void release_world_connection(uint32_t world_id, PGconn* conn) {
    WorldDatabasePool* pool = pool_for(world_id);
    if (!pool || !conn) return;

    pthread_mutex_lock(&pool->pool_lock);

    for (int i = 0; i < pool->conn_count; i++) {
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
        LOG_ERROR("Failed to get character list for world %u: %s",
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

    LOG_INFO("Retrieved %d characters for account %u from world '%s'",
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
        LOG_ERROR("Failed to count characters for world %u: %s",
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
        LOG_ERROR("Failed to create character in world %u: %s",
                  world_id, PQerrorMessage(conn));
        PQclear(res);
        PQclear(PQexec(conn, "ROLLBACK"));
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
        LOG_ERROR("Failed to commit character '%s' in world %u: %s",
                  name, world_id, PQerrorMessage(conn));
        PQclear(res);
        PQclear(PQexec(conn, "ROLLBACK"));
        release_world_connection(world_id, conn);
        return 0;
    }
    PQclear(res);

    release_world_connection(world_id, conn);

    LOG_INFO("Created character '%s' (ID: %u) in world '%s'",
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

    char world_id_str[32];
    snprintf(world_id_str, sizeof(world_id_str), "%u", world_id);

    const char* param_values[3] = {character_id_str, account_id_str, world_id_str};

    // world_id is part of the predicate, not just of the connection choice.
    // The signature has always accepted it; the query used to ignore it, which
    // is harmless only while every world owns its own database. The moment two
    // worlds share one, an unscoped ownership check answers "yes" for a
    // character the account owns in a *different* world -- and this function
    // is what gates world entry and character deletion.
    PGresult* res = PQexecParams(conn,
        "SELECT 1 FROM characters "
        "WHERE character_id = $1 AND account_id = $2 AND world_id = $3",
        3, NULL, param_values, NULL, NULL, 0
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

    if (!success) {
        // Nothing was deleted, so there is nothing to commit; roll back rather
        // than leaving the connection inside an open transaction for whoever
        // takes it out of the pool next.
        res = PQexec(conn, "ROLLBACK");
        PQclear(res);
        release_world_connection(world_id, conn);
        return 0;
    }

    // The COMMIT result was discarded here, exactly as it was in
    // world_character_create() before that one was fixed: the DELETE row count
    // said "one row went away", the commit then failed, and the caller told
    // the client the character was gone while it was still in the table.
    res = PQexec(conn, "COMMIT");
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        LOG_ERROR("Failed to commit deletion of character %u in world %u: %s",
                  character_id, world_id, PQerrorMessage(conn));
        PQclear(res);
        res = PQexec(conn, "ROLLBACK");
        PQclear(res);
        release_world_connection(world_id, conn);
        return 0;
    }
    PQclear(res);

    release_world_connection(world_id, conn);

    if (success) {
        LOG_INFO("Deleted character %u from world '%s'",
                 character_id, get_world_name_by_id(world_id));
    }

    return success;
}

/** Close all world pools and destroy their synchronization objects.
 *
 * Guarded on g_pools_ready rather than on each pool's initialized flag: the
 * synchronization objects now belong to the process, not to a successful
 * connect, so a pool that never opened still owns objects worth destroying —
 * and one that never ran pools_init_once() owns none.
 */
void world_databases_cleanup(void) {
    if (!g_pools_ready) {
        LOG_WARN("No world database pools to clean up");
        return;
    }

    for (size_t w = 0; w < g_pool_count; w++) {
        WorldDatabasePool* pool = &g_world_pools[w];

        for (int i = 0; i < pool->conn_count; i++) {
            if (pool->connections[i].conn) {
                PQfinish(pool->connections[i].conn);
                pool->connections[i].conn = NULL;
            }
            pthread_mutex_destroy(&pool->connections[i].lock);
        }
        free(pool->connections);
        pool->connections = NULL;
        pool->conn_count = 0;
        pthread_cond_destroy(&pool->conn_available);
        pthread_mutex_destroy(&pool->pool_lock);
        pthread_mutex_destroy(&pool->init_lock);
        atomic_store(&pool->initialized, 0);
    }

    free(g_world_pools);
    g_world_pools = NULL;
    g_pool_count  = 0;
    g_pools_ready = 0;
    LOG_INFO("Cleaned up all world database connections");
}
