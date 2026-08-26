/**
 * @file
 * Manage pooled PostgreSQL access for character records and persistent item instances.
 */

#include "players_database.h"
#include "schema_migrations.h"
#include "log.h"
#include "class_stats.h"
#include "world_regions.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>

/* The default pool size is public: see DB_CONN_POOL_SIZE in
 * players_database.h. The pool that is actually opened is g_pool.size, read
 * from $MMO_DB_POOL_SIZE at init. */
#define CONN_ACQUIRE_TIMEOUT_MS 5000

/** Track one PostgreSQL connection and its checkout state. */
typedef struct {
    PGconn* conn;
    int in_use;
    pthread_mutex_t lock;
} PooledConnection;

/** Own the fixed connection pool and its availability synchronization.
 *
 * `waiters` counts threads currently parked in pthread_cond_timedwait() on
 * conn_available. character_database_close() destroys pool_lock and
 * conn_available, and destroying a mutex or condition variable another thread
 * is blocked on is undefined behaviour -- previously that was safe only
 * because of the order the services happen to join their threads in, which is
 * a dependency nothing stated and nothing enforced. Close now drains the
 * waiters explicitly and refuses to destroy anything until the count is zero.
 */
typedef struct {
    /** `size` slots, allocated at init.
     *
     * Heap rather than a fixed array so the count is a deployment decision
     * rather than a compile-time one. It is the real concurrency limit of
     * every service that touches character data, and the right number depends
     * on how many workers can be in a character query at once and on what the
     * database server will accept -- neither of which this build can know.
     * The realm's per-world pools have been sized this way for a while; this
     * is the same arrangement for the world's own pool.
     */
    PooledConnection* connections;
    int size;
    char connection_string[512];
    int initialized;
    int waiters;
    pthread_mutex_t pool_lock;
    pthread_cond_t conn_available;
    pthread_cond_t pool_drained;
} ConnectionPool;

static ConnectionPool g_pool = {0};

static PGconn* acquire_connection(void);
static void release_connection(PGconn* conn);
static int reconnect_if_needed(PooledConnection* pc);
static int configured_pool_size(void);

/* --- The schema ---------------------------------------------------------- *
 *
 * One numbered migration per change, applied in order and recorded. See
 * schema_migrations.h for the rules; the two that matter most are that an
 * applied migration is never edited and never renumbered.
 *
 * Migration 1 is the baseline: the entire schema as it stood when versioning
 * was introduced, written idempotently so it applies cleanly both to an empty
 * database and to every existing deployment, which already has all of it. On
 * an existing database it changes nothing and simply records that the database
 * is at version 1.
 *
 * CURRENCY_ENNARA is spelled out as a literal in the seeding statement rather
 * than formatted in at runtime. A migration is a historical record: if the
 * enum's value were ever renumbered, the SQL that already ran against live
 * databases would silently start meaning something else.
 */
static const SchemaMigration character_migrations[] = {
    {
        .version = 1,
        .name    = "baseline character schema",
        .up =
            /* Characters. */
            "CREATE TABLE IF NOT EXISTS characters ("
            "    character_id SERIAL PRIMARY KEY,"
            "    account_id INTEGER NOT NULL,"
            "    world_id INTEGER NOT NULL,"
            "    name VARCHAR(32) NOT NULL,"
            "    class_id INTEGER NOT NULL,"
            "    race_id INTEGER NOT NULL,"
            "    level INTEGER DEFAULT 1,"
            "    first_load INTEGER DEFAULT 1,"
            "    pos_x REAL DEFAULT 0.0,"
            "    pos_y REAL DEFAULT 0.0,"
            "    health INTEGER DEFAULT 100,"
            "    max_health INTEGER DEFAULT 100,"
            "    experience BIGINT DEFAULT 0,"
            "    gold INTEGER DEFAULT 0,"
            "    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,"
            "    UNIQUE(name, world_id)"
            ");"

            /* Equipment used to be nine columns on `characters`; it is
             * character_items now. Dropping them is why this migration has no
             * `down`. */
            "ALTER TABLE characters "
            "  DROP COLUMN IF EXISTS helmet,"
            "  DROP COLUMN IF EXISTS gloves,"
            "  DROP COLUMN IF EXISTS chest_armor,"
            "  DROP COLUMN IF EXISTS leggings,"
            "  DROP COLUMN IF EXISTS boots,"
            "  DROP COLUMN IF EXISTS main_hand,"
            "  DROP COLUMN IF EXISTS second_hand,"
            "  DROP COLUMN IF EXISTS effect,"
            "  DROP COLUMN IF EXISTS inventory;"

            "ALTER TABLE characters ADD COLUMN IF NOT EXISTS mana INTEGER DEFAULT 100;"
            "ALTER TABLE characters ADD COLUMN IF NOT EXISTS max_mana INTEGER DEFAULT 100;"

            /* Item instances. durability and rolled_stats are reserved
             * nullable columns for item instancing that is not built yet. */
            "CREATE TABLE IF NOT EXISTS character_items ("
            "    instance_id    BIGINT PRIMARY KEY,"
            "    character_id   INTEGER NOT NULL,"
            "    slot           SMALLINT NOT NULL,"
            "    item_id        INTEGER NOT NULL,"
            "    quantity       SMALLINT NOT NULL DEFAULT 1 CHECK (quantity > 0),"
            "    is_bound       BOOLEAN NOT NULL DEFAULT FALSE,"
            "    durability     SMALLINT,"
            "    max_durability SMALLINT,"
            "    rolled_stats   JSONB,"
            "    created_at     TIMESTAMP DEFAULT CURRENT_TIMESTAMP,"
            "    UNIQUE (character_id, slot)"
            ");"

            /* One row per character per kingdom currency. A table rather than
             * a column per currency, so adding a kingdom needs no schema
             * change at all. */
            "CREATE TABLE IF NOT EXISTS character_currencies ("
            "    character_id INTEGER NOT NULL,"
            "    currency_id  SMALLINT NOT NULL,"
            "    amount       BIGINT NOT NULL DEFAULT 0 CHECK (amount >= 0),"
            "    PRIMARY KEY (character_id, currency_id)"
            ");"

            /* Seed Ennara (currency 0) from the legacy single-gold column.
             * Existing rows win, so this cannot overwrite a live balance. The
             * `gold` column is deliberately left in place, unread and
             * unwritten, so a pre-currency build still finds its data. */
            "INSERT INTO character_currencies (character_id, currency_id, amount) "
            "SELECT character_id, 0, gold FROM characters WHERE gold > 0 "
            "ON CONFLICT (character_id, currency_id) DO NOTHING;"

            /* Orphans first: ADD CONSTRAINT refuses to validate against rows
             * that already violate it. These exist because there was no
             * foreign key, so a deleted character left its items behind
             * forever -- still owning instance ids and still occupying
             * UNIQUE(character_id, slot) for a character that was gone. */
            "DELETE FROM character_items i "
            "WHERE NOT EXISTS (SELECT 1 FROM characters c "
            "                  WHERE c.character_id = i.character_id);"
            "DELETE FROM character_currencies cc "
            "WHERE NOT EXISTS (SELECT 1 FROM characters c "
            "                  WHERE c.character_id = cc.character_id);"

            /* A DO block rather than ADD CONSTRAINT IF NOT EXISTS, which
             * PostgreSQL does not have for table constraints. */
            "DO $$ BEGIN "
            "  IF NOT EXISTS (SELECT 1 FROM pg_constraint "
            "                 WHERE conname = 'character_items_owner_fk') THEN "
            "    ALTER TABLE character_items ADD CONSTRAINT character_items_owner_fk "
            "      FOREIGN KEY (character_id) REFERENCES characters(character_id) "
            "      ON DELETE CASCADE; "
            "  END IF; "
            "  IF NOT EXISTS (SELECT 1 FROM pg_constraint "
            "                 WHERE conname = 'character_currencies_owner_fk') THEN "
            "    ALTER TABLE character_currencies ADD CONSTRAINT character_currencies_owner_fk "
            "      FOREIGN KEY (character_id) REFERENCES characters(character_id) "
            "      ON DELETE CASCADE; "
            "  END IF; "
            "END $$;"

            "CREATE INDEX IF NOT EXISTS idx_character_items_owner "
            "ON character_items(character_id);"
            "CREATE INDEX IF NOT EXISTS idx_characters_account_world "
            "ON characters(account_id, world_id);",

        /* No rollback. This step drops the nine legacy equipment columns and
         * deletes orphaned rows; nothing can put either back, and a `down`
         * that recreated empty columns would be a lie about what it restored. */
        .down = NULL,
    },
};


/**
 * Initialize the PostgreSQL pool and required character tables and indexes.
 *
 * Performs blocking connection and schema operations.
 *
 * @return 1 when initialized or already active, or 0 on connection or required-schema failure.
 */
int character_database_init(const char* connection_string) {
    if (g_pool.initialized) {
        LOG_ERROR("Database already initialized");
        return 1;
    }

    g_pool.size = configured_pool_size();
    g_pool.connections = calloc((size_t)g_pool.size, sizeof(PooledConnection));
    if (!g_pool.connections) {
        LOG_ERROR("Out of memory allocating a %d-connection pool", g_pool.size);
        return 0;
    }

    pthread_mutex_init(&g_pool.pool_lock, NULL);
    pthread_cond_init(&g_pool.conn_available, NULL);
    pthread_cond_init(&g_pool.pool_drained, NULL);
    g_pool.waiters = 0;

    strncpy(g_pool.connection_string, connection_string, sizeof(g_pool.connection_string) - 1);
    g_pool.connection_string[sizeof(g_pool.connection_string) - 1] = '\0';

    // Initialize connection pool
    for (int i = 0; i < g_pool.size; i++) {
        pthread_mutex_init(&g_pool.connections[i].lock, NULL);
        g_pool.connections[i].in_use = 0;
        g_pool.connections[i].conn = PQconnectdb(connection_string);

        if (PQstatus(g_pool.connections[i].conn) != CONNECTION_OK) {
            LOG_ERROR("PostgreSQL connection %d failed: %s",
                      i, PQerrorMessage(g_pool.connections[i].conn));

            // Clean up the failed connection and all previously created ones.
            PQfinish(g_pool.connections[i].conn);
            g_pool.connections[i].conn = NULL;
            pthread_mutex_destroy(&g_pool.connections[i].lock);
            for (int j = 0; j < i; j++) {
                if (g_pool.connections[j].conn) {
                    PQfinish(g_pool.connections[j].conn);
                }
                pthread_mutex_destroy(&g_pool.connections[j].lock);
            }
            pthread_mutex_destroy(&g_pool.pool_lock);
            pthread_cond_destroy(&g_pool.conn_available);
            pthread_cond_destroy(&g_pool.pool_drained);
            free(g_pool.connections);
            g_pool.connections = NULL;
            g_pool.size = 0;
            return 0;
        }

        // Set connection to non-blocking mode for better timeout handling
        PQsetnonblocking(g_pool.connections[i].conn, 0);
    }

    g_pool.initialized = 1;

    /* Bring the schema to the version this build expects.
     *
     * Everything that used to happen here -- a CREATE TABLE IF NOT EXISTS per
     * table and a run of ad-hoc ALTERs, re-executed on every startup, with
     * failures logged as warnings and carried on from -- is now a numbered
     * migration in character_migrations[] below, applied once, recorded in
     * schema_version, and fatal when it fails.
     *
     * A single connection is taken for the whole run: schema_migrate() holds a
     * session-scoped advisory lock, and a session lock belongs to the
     * connection that took it. */
    PGconn* setup_conn = acquire_connection();
    if (!setup_conn) {
        LOG_ERROR("Failed to acquire connection for schema migration");
        character_database_close();
        return 0;
    }

    if (!schema_migrate(setup_conn, character_migrations,
                        sizeof(character_migrations) / sizeof(character_migrations[0]),
                        "characters")) {
        /* Fatal, and deliberately so. The old code would warn and continue,
         * which meant a world could run for weeks against a database missing a
         * column the code assumed was there -- and only find out when a player
         * lost something. */
        LOG_ERROR("Schema migration failed; refusing to start against a "
                  "database in an unknown state");
        release_connection(setup_conn);
        character_database_close();
        return 0;
    }

    release_connection(setup_conn);

    LOG_INFO("PostgreSQL character database initialized successfully with %d connections",
             g_pool.size);
    return 1;
}

void character_database_pool_stats(int* out_in_use, int* out_size,
                                   int* out_waiters) {
    if (out_size)    *out_size    = g_pool.size;
    if (out_in_use)  *out_in_use  = 0;
    if (out_waiters) *out_waiters = 0;

    if (!g_pool.initialized) return;

    /* Read under the pool lock but without touching a slot lock: this answers
     * a metrics scrape, and a scrape must never be able to stall a query. */
    pthread_mutex_lock(&g_pool.pool_lock);
    int in_use = 0;
    for (int i = 0; i < g_pool.size; i++) {
        if (g_pool.connections[i].in_use) in_use++;
    }
    if (out_in_use)  *out_in_use  = in_use;
    if (out_waiters) *out_waiters = g_pool.waiters;
    pthread_mutex_unlock(&g_pool.pool_lock);
}

/**
 * Close every pooled connection and destroy pool synchronization objects.
 */
void character_database_close(void) {
    if (!g_pool.initialized) {
        return;
    }

    pthread_mutex_lock(&g_pool.pool_lock);

    /* Publish the shutdown before touching a single connection. Every waiter
     * re-checks `initialized` when it wakes and leaves with NULL rather than
     * being handed a connection that is about to be finished. */
    g_pool.initialized = 0;
    pthread_cond_broadcast(&g_pool.conn_available);

    /* Wait for the parked threads to actually leave the condition variable.
     * Bounded, because a shutdown that hangs forever is worse than one that
     * reports the problem: a waiter that has not gone by then is stuck on
     * something other than this pool, and its 5s acquire timeout will expire
     * on its own. */
    while (g_pool.waiters > 0) {
        struct timespec drain_deadline;
        clock_gettime(CLOCK_REALTIME, &drain_deadline);
        drain_deadline.tv_sec += (CONN_ACQUIRE_TIMEOUT_MS / 1000) + 1;
        if (pthread_cond_timedwait(&g_pool.pool_drained, &g_pool.pool_lock,
                                   &drain_deadline) == ETIMEDOUT) {
            LOG_ERROR("Closing the connection pool with %d thread(s) still "
                      "waiting on it; leaking its synchronization objects "
                      "rather than destroying one in use",
                      g_pool.waiters);
            break;
        }
    }

    int leaked = (g_pool.waiters > 0);

    for (int i = 0; i < g_pool.size; i++) {
        pthread_mutex_lock(&g_pool.connections[i].lock);
        if (g_pool.connections[i].conn) {
            PQfinish(g_pool.connections[i].conn);
            g_pool.connections[i].conn = NULL;
        }
        pthread_mutex_unlock(&g_pool.connections[i].lock);
        if (!leaked) {
            pthread_mutex_destroy(&g_pool.connections[i].lock);
        }
    }

    pthread_mutex_unlock(&g_pool.pool_lock);

    if (!leaked) {
        pthread_mutex_destroy(&g_pool.pool_lock);
        pthread_cond_destroy(&g_pool.conn_available);
        pthread_cond_destroy(&g_pool.pool_drained);

        /* Only when nothing is parked on the pool. A waiter that outlived the
         * drain above still holds a pointer into this array; leaking it is the
         * lesser fault, and the message above has already said so. */
        free(g_pool.connections);
        g_pool.connections = NULL;
        g_pool.size = 0;
    }

    LOG_INFO("PostgreSQL connection pool closed");
}

/**
 * Resolve the pool size from the environment.
 *
 * Reads `$MMO_DB_POOL_SIZE`, defaulting to DB_CONN_POOL_SIZE. This is the real
 * concurrency limit of every service that touches character data --
 * acquire_connection() waits five seconds past it and then fails the query --
 * and the right value depends on the worker count in front of it and on what
 * the database server will accept, neither of which is a compile-time fact.
 *
 * The upper bound is a sanity ceiling, not a recommendation: PostgreSQL's own
 * max_connections is the number that actually matters, and it is shared with
 * every other process pointed at the same server.
 *
 * @return The configured size, or DB_CONN_POOL_SIZE when unset or unusable.
 */
static int configured_pool_size(void) {
    const char* value = getenv("MMO_DB_POOL_SIZE");
    if (!value || !*value) return DB_CONN_POOL_SIZE;

    char* end = NULL;
    long parsed = strtol(value, &end, 10);
    if (end == value || *end || parsed < 1 || parsed > 1024) {
        LOG_ERROR("MMO_DB_POOL_SIZE='%s' is not a usable pool size; using %d",
                  value, DB_CONN_POOL_SIZE);
        return DB_CONN_POOL_SIZE;
    }
    return (int)parsed;
}

/** Report the pool size this process will open, before it has opened it.
 *
 * Callers that size a worker pool in front of the database need the number
 * before character_database_init() has run -- and running more workers than
 * connections converts a harmless wait into a query that times out under
 * exactly the load that caused it. Answers from the environment, so it agrees
 * with what init will do.
 */
int character_database_pool_configured_size(void) {
    return g_pool.size > 0 ? g_pool.size : configured_pool_size();
}

/**
 * Reset a pooled PostgreSQL connection when its status is unhealthy.
 *
 * @return 1 when connected or successfully reset, or 0 otherwise.
 */
static int reconnect_if_needed(PooledConnection* pc) {
    if (PQstatus(pc->conn) == CONNECTION_OK) {
        return 1;
    }

    LOG_ERROR("Connection lost, attempting to reconnect...");
    PQreset(pc->conn);

    if (PQstatus(pc->conn) == CONNECTION_OK) {
        LOG_INFO("Successfully reconnected");
        return 1;
    }

    LOG_ERROR("Reconnection failed: %s", PQerrorMessage(pc->conn));
    return 0;
}

/**
 * Acquire a healthy pooled connection with a bounded wait.
 *
 * The caller must return a successful result with release_connection().
 *
 * @return A checked-out connection, or NULL when uninitialized, reconnection fails, or the wait times out.
 */
static PGconn* acquire_connection(void) {
    struct timespec timeout;
    clock_gettime(CLOCK_REALTIME, &timeout);
    timeout.tv_sec += CONN_ACQUIRE_TIMEOUT_MS / 1000;
    timeout.tv_nsec += (CONN_ACQUIRE_TIMEOUT_MS % 1000) * 1000000;
    if (timeout.tv_nsec >= 1000000000) {
        timeout.tv_sec++;
        timeout.tv_nsec -= 1000000000;
    }

    pthread_mutex_lock(&g_pool.pool_lock);

    if (!g_pool.initialized) {
        pthread_mutex_unlock(&g_pool.pool_lock);
        return NULL;
    }

    while (1) {
        int unhealthy = 0;

        // Try to find an available connection
        for (int i = 0; i < g_pool.size; i++) {
            if (!g_pool.connections[i].in_use) {
                pthread_mutex_lock(&g_pool.connections[i].lock);

                // Double-check after acquiring lock
                if (!g_pool.connections[i].in_use) {
                    g_pool.connections[i].in_use = 1;

                    // Verify connection is good
                    if (!reconnect_if_needed(&g_pool.connections[i])) {
                        /* Put the slot back and say so. Releasing in_use
                         * without signalling left a waiter asleep for its
                         * full 5s timeout on a connection that was already
                         * free -- the pool had capacity and nobody was told.
                         *
                         * And carry on down the pool rather than failing the
                         * whole request: one dead server-side connection used
                         * to fail a caller that nine healthy ones could have
                         * served. */
                        g_pool.connections[i].in_use = 0;
                        pthread_mutex_unlock(&g_pool.connections[i].lock);
                        pthread_cond_signal(&g_pool.conn_available);
                        unhealthy = 1;
                        continue;
                    }

                    pthread_mutex_unlock(&g_pool.connections[i].lock);
                    pthread_mutex_unlock(&g_pool.pool_lock);
                    return g_pool.connections[i].conn;
                }

                pthread_mutex_unlock(&g_pool.connections[i].lock);
            }
        }

        /* Every free slot in this pass failed to reconnect. Waiting on the
         * condition variable would only park us until the timeout while
         * re-scanning would spin on PQreset(), so report the outage now. */
        if (unhealthy) {
            pthread_mutex_unlock(&g_pool.pool_lock);
            LOG_ERROR("No healthy database connection available");
            return NULL;
        }

        // No connections available, wait with timeout
        g_pool.waiters++;
        int wait_result = pthread_cond_timedwait(&g_pool.conn_available,
                                                  &g_pool.pool_lock, &timeout);
        g_pool.waiters--;

        /* character_database_close() sets initialized to 0 and broadcasts;
         * it then blocks until this count reaches zero, so the signal has to
         * go out on every exit from the wait, not only on the shutdown path. */
        if (g_pool.waiters == 0) {
            pthread_cond_signal(&g_pool.pool_drained);
        }

        if (!g_pool.initialized) {
            pthread_mutex_unlock(&g_pool.pool_lock);
            return NULL;
        }

        if (wait_result == ETIMEDOUT) {
            pthread_mutex_unlock(&g_pool.pool_lock);
            LOG_ERROR("Timeout waiting for database connection");
            return NULL;
        }
    }
}

/**
 * Return a checked-out connection and signal one waiter.
 */
static void release_connection(PGconn* conn) {
    if (!conn) return;

    pthread_mutex_lock(&g_pool.pool_lock);

    for (int i = 0; i < g_pool.size; i++) {
        if (g_pool.connections[i].conn == conn) {
            pthread_mutex_lock(&g_pool.connections[i].lock);
            g_pool.connections[i].in_use = 0;
            pthread_mutex_unlock(&g_pool.connections[i].lock);

            // Signal that a connection is available
            pthread_cond_signal(&g_pool.conn_available);
            break;
        }
    }

    pthread_mutex_unlock(&g_pool.pool_lock);
}

/**
 * Load up to ten characters belonging to an account in one world.
 *
 * @param characters  Output array with room for max_count records.
 * @param max_count   Output capacity; must be positive.
 * @return            The number loaded, or 0 on invalid input or query failure.
 */
int character_get_list_for_world(uint32_t account_id, uint32_t world_id,
                                  CharacterInfo* characters, int max_count) {
    if (!characters || max_count <= 0) {
        return 0;
    }

    PGconn* conn = acquire_connection();
    if (!conn) {
        LOG_ERROR("Failed to acquire connection for character list");
        return 0;
    }

    char account_id_str[32];
    char world_id_str[32];
    snprintf(account_id_str, sizeof(account_id_str), "%u", account_id);
    snprintf(world_id_str, sizeof(world_id_str), "%u", world_id);

    const char* param_values[2] = {account_id_str, world_id_str};

    PGresult* res = PQexecParams(conn,
            "SELECT character_id, name, level, class_id, race_id, "
            "       pos_x, pos_y, health, max_health "
            "FROM characters "
            "WHERE account_id = $1 AND world_id = $2 "
            "ORDER BY created_at DESC "
            "LIMIT 10",
            2,
            NULL,
            param_values,
            NULL, NULL, 0
        );

    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        LOG_ERROR("Failed to get character list: %s", PQerrorMessage(conn));
        PQclear(res);
        release_connection(conn);
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
        characters[i].pos_x     = (float)strtod(PQgetvalue(res, i, 5), NULL);
        characters[i].pos_y     = (float)strtod(PQgetvalue(res, i, 6), NULL);
        characters[i].health    = (uint32_t)strtoul(PQgetvalue(res, i, 7), NULL, 10);
        characters[i].max_health = (uint32_t)strtoul(PQgetvalue(res, i, 8), NULL, 10);
    }

    PQclear(res);
    release_connection(conn);
    return count;
}

/**
 * Count an account's characters in one world.
 *
 * @return The count, or 0 on query failure.
 */
int character_count_in_world(uint32_t account_id, uint32_t world_id) {
    PGconn* conn = acquire_connection();
    if (!conn) {
        LOG_ERROR("Failed to acquire connection for character count");
        return 0;
    }

    char account_id_str[32];
    char world_id_str[32];
    snprintf(account_id_str, sizeof(account_id_str), "%u", account_id);
    snprintf(world_id_str, sizeof(world_id_str), "%u", world_id);

    const char* param_values[2] = {account_id_str, world_id_str};

    PGresult* res = PQexecParams(conn,
        "SELECT COUNT(*) FROM characters WHERE account_id = $1 AND world_id = $2",
        2,
        NULL,
        param_values,
        NULL, NULL, 0
    );

    if (PQresultStatus(res) != PGRES_TUPLES_OK || PQntuples(res) == 0) {
        PQclear(res);
        release_connection(conn);
        return 0;
    }

    int count = atoi(PQgetvalue(res, 0, 0));
    PQclear(res);
    release_connection(conn);

    return count;
}

/**
 * Create a level-one character with class-derived health and mana.
 *
 * @param name              Character name; may not be NULL.
 * @param class_id          Class used to derive initial statistics.
 * @param race_id           Stored race identifier.
 * @param out_character_id  Receives the generated identifier; may not be NULL.
 * @return                  1 after a committed insert, or 0 on validation or database failure.
 */
int character_create_in_world(uint32_t account_id, uint32_t world_id,
                              const char* name, int class_id, int race_id,
                              uint32_t* out_character_id) {
    if (!name || !out_character_id) {
        return 0;
    }

    PGconn* conn = acquire_connection();
    if (!conn) {
        LOG_ERROR("Failed to acquire connection for character creation");
        return 0;
    }

    // Begin transaction for atomicity
    PGresult* res = PQexec(conn, "BEGIN");
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        LOG_ERROR("Failed to begin transaction: %s", PQerrorMessage(conn));
        PQclear(res);
        release_connection(conn);
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

    /* Compute the level-1 starting values from the race registry. A race that is not
     * loaded cannot produce them, and a character created anyway would start with zero
     * health, so refuse the create instead of writing a broken row. */
    DerivedStats initial_stats;
    if (!class_stats_compute((uint32_t)class_id, 1, &initial_stats)) {
        LOG_ERROR("Refusing to create '%s': race %d is not in the registry",
                  name, class_id);
        release_connection(conn);
        return 0;
    }

    char health_str[32], max_health_str[32];
    char mana_str[32], max_mana_str[32];

    snprintf(health_str, sizeof(health_str), "%d", initial_stats.max_health);
    snprintf(max_health_str, sizeof(max_health_str), "%d", initial_stats.max_health);
    /* The mana columns hold whichever resource the character's role selects. The
     * schema keeps its original column names; only their meaning generalised. */
    snprintf(mana_str, sizeof(mana_str), "%d", initial_stats.max_resource);
    snprintf(max_mana_str, sizeof(max_mana_str), "%d", initial_stats.max_resource);

    const char* param_values[9] = {
        account_id_str,     // $1
        world_id_str,       // $2
        name,               // $3
        class_id_str,       // $4
        race_id_str,        // $5
        health_str,         // $6
        max_health_str,     // $7
        mana_str,           // $8
        max_mana_str        // $9
    };

    res = PQexecParams(conn,
        "INSERT INTO characters (account_id, world_id, name, class_id, race_id, "
        "health, max_health, mana, max_mana) "
        "VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9) "
        "RETURNING character_id",
        9, NULL, param_values, NULL, NULL, 0);

    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        LOG_ERROR("Failed to create character: %s", PQerrorMessage(conn));
        PQclear(res);
        PQclear(PQexec(conn, "ROLLBACK"));
        release_connection(conn);
        return 0;
    }

    if (PQntuples(res) == 0) {
        LOG_ERROR("Character creation returned no ID");
        PQclear(res);
        PQclear(PQexec(conn, "ROLLBACK"));
        release_connection(conn);
        return 0;
    }

    *out_character_id = (uint32_t)strtoul(PQgetvalue(res, 0, 0), NULL, 10);
    PQclear(res);

    // Commit transaction
    res = PQexec(conn, "COMMIT");
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        LOG_ERROR("Failed to commit transaction: %s", PQerrorMessage(conn));
        PQclear(res);
        PQclear(PQexec(conn, "ROLLBACK"));
        release_connection(conn);
        return 0;
    }
    PQclear(res);

    LOG_INFO("Successfully created character '%s' (race %d) with ID %u for account %u in world %u (hp=%d, resource=%d)",
             name, class_id, *out_character_id, account_id, world_id,
             initial_stats.max_health, initial_stats.max_resource);

    release_connection(conn);
    return 1;
}

/**
 * Check whether a character belongs to an account.
 *
 * @return 1 when ownership matches, or 0 for absence or query failure.
 */
int character_belongs_to_account(uint32_t character_id, uint32_t account_id) {
    PGconn* conn = acquire_connection();
    if (!conn) {
        LOG_ERROR("Failed to acquire connection for ownership check");
        return 0;
    }

    char character_id_str[32];
    char account_id_str[32];
    snprintf(character_id_str, sizeof(character_id_str), "%u", character_id);
    snprintf(account_id_str, sizeof(account_id_str), "%u", account_id);

    const char* param_values[2] = {character_id_str, account_id_str};

    PGresult* res = PQexecParams(conn,
        "SELECT 1 FROM characters WHERE character_id = $1 AND account_id = $2",
        2,
        NULL,
        param_values,
        NULL, NULL, 0
    );

    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        PQclear(res);
        release_connection(conn);
        return 0;
    }

    int belongs = (PQntuples(res) > 0);
    PQclear(res);
    release_connection(conn);

    return belongs;
}

/**
 * Load a character's scalar state and clear its embedded item arrays.
 *
 * @param char_info  Receives the character record; may not be NULL.
 * @return           1 when found and loaded, or 0 otherwise.
 */
int character_get_full_data(uint32_t character_id, CharacterInfo* char_info) {
    if (!char_info) {
        LOG_ERROR("character_get_full_data: NULL char_info pointer");
        return 0;
    }

    PGconn* conn = acquire_connection();
    if (!conn) {
        LOG_ERROR("Failed to acquire connection for full character data");
        return 0;
    }

    char character_id_str[32];
    snprintf(character_id_str, sizeof(character_id_str), "%u", character_id);

    const char* param_values[1] = {character_id_str};

    PGresult* res = PQexecParams(conn,
        "SELECT character_id, name, level, class_id, race_id, "
        "       pos_x, pos_y, health, max_health, mana, max_mana, experience "
        "FROM characters "
        "WHERE character_id = $1",
        1,
        NULL,
        param_values,
        NULL, NULL, 0
    );

    if (PQresultStatus(res) != PGRES_TUPLES_OK || PQntuples(res) == 0) {
        LOG_ERROR("Failed to get full character data: %s", PQerrorMessage(conn));
        PQclear(res);
        release_connection(conn);
        return 0;
    }

    // Parse all fields
    char_info->character_id = (uint32_t)strtoul(PQgetvalue(res, 0, 0), NULL, 10);
    strncpy(char_info->name, PQgetvalue(res, 0, 1), 31);
    char_info->name[31] = '\0';
    char_info->level = (uint32_t)strtoul(PQgetvalue(res, 0, 2), NULL, 10);
    char_info->race_id = (uint32_t)strtoul(PQgetvalue(res, 0, 3), NULL, 10);
    char_info->pos_x = (float)strtod(PQgetvalue(res, 0, 5), NULL);
    char_info->pos_y = (float)strtod(PQgetvalue(res, 0, 6), NULL);
    char_info->health = (uint32_t)strtoul(PQgetvalue(res, 0, 7), NULL, 10);
    char_info->max_health = (uint32_t)strtoul(PQgetvalue(res, 0, 8), NULL, 10);
    char_info->resource = (int32_t)strtol(PQgetvalue(res, 0, 9), NULL, 10);
    char_info->max_resource = (int32_t)strtol(PQgetvalue(res, 0, 10), NULL, 10);
    char_info->experience = (uint64_t)strtoull(PQgetvalue(res, 0, 11), NULL, 10);

    // item arrays load separately from character_items
    memset(char_info->inventory, 0, sizeof(char_info->inventory));
    memset(char_info->equipment, 0, sizeof(char_info->equipment));

    PQclear(res);

    /* Balances live in character_currencies. A character with no rows yet is
     * simply broke, which is not a load failure. */
    memset(char_info->currency, 0, sizeof(char_info->currency));
    res = PQexecParams(conn,
        "SELECT currency_id, amount FROM character_currencies WHERE character_id = $1",
        1, NULL, param_values, NULL, NULL, 0);

    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        LOG_ERROR("Failed to load currencies for character %u: %s",
                  character_id, PQerrorMessage(conn));
        PQclear(res);
        release_connection(conn);
        return 0;
    }

    int currency_rows = PQntuples(res);
    for (int i = 0; i < currency_rows; i++) {
        int      id     = atoi(PQgetvalue(res, i, 0));
        uint64_t amount = strtoull(PQgetvalue(res, i, 1), NULL, 10);

        /* A row for a currency this build does not know belongs to a newer
         * build. Skip it rather than dropping it: the save path only writes
         * the currencies it knows, so the unknown row survives untouched. */
        if (!world_currency_valid(id)) {
            LOG_ERROR("character %u holds unknown currency %d — ignoring",
                      character_id, id);
            continue;
        }
        if (amount > UINT32_MAX) amount = UINT32_MAX;
        char_info->currency[id] = (uint32_t)amount;
    }

    PQclear(res);
    release_connection(conn);

    return 1;
}

/** Run a transaction-control statement and release its result.
 *
 * PQexec returns a PGresult for BEGIN, COMMIT and ROLLBACK the same as for any
 * other statement, and every one of those results used to be discarded without
 * PQclear -- a leak on every save that took a failure path.
 *
 * @return 1 when the command succeeded, otherwise 0.
 */
static int tx_exec(PGconn* conn, const char* command) {
    PGresult* res = PQexec(conn, command);
    int ok = (PQresultStatus(res) == PGRES_COMMAND_OK);
    if (!ok)
        LOG_ERROR("%s failed: %s", command, PQerrorMessage(conn));
    PQclear(res);
    return ok;
}

/**
 * Write a character's scalars and currency balances inside an open transaction.
 *
 * The caller owns the transaction. Split out of character_update_full_data() so
 * the same statements can run inside a transaction that also carries the item
 * writes -- see character_save_all().
 *
 * @return 1 on success, or 0 on database failure.
 */
static int update_scalars_tx(PGconn* conn, const CharacterInfo* char_info) {
    // item instances persist through the item writes
    char character_id_str[32], level_str[32], pos_x_str[32], pos_y_str[32];
    char health_str[32], max_health_str[32], mana_str[32], max_mana_str[32];
    char experience_str[32];

    snprintf(character_id_str, sizeof(character_id_str), "%u", char_info->character_id);
    snprintf(level_str, sizeof(level_str), "%u", char_info->level);
    snprintf(pos_x_str, sizeof(pos_x_str), "%f", char_info->pos_x);
    snprintf(pos_y_str, sizeof(pos_y_str), "%f", char_info->pos_y);
    snprintf(health_str, sizeof(health_str), "%u", char_info->health);
    snprintf(max_health_str, sizeof(max_health_str), "%u", char_info->max_health);
    snprintf(mana_str, sizeof(mana_str), "%d", char_info->resource);
    snprintf(max_mana_str, sizeof(max_mana_str), "%d", char_info->max_resource);
    snprintf(experience_str, sizeof(experience_str), "%lu", char_info->experience);

    const char* param_values[9] = {
        level_str,          // $1
        pos_x_str,          // $2
        pos_y_str,          // $3
        health_str,         // $4
        max_health_str,     // $5
        mana_str,           // $6
        max_mana_str,       // $7
        experience_str,     // $8
        character_id_str    // $9
    };

    PGresult* res = PQexecParams(conn,
        "UPDATE characters SET "
        "level = $1, pos_x = $2, pos_y = $3, "
        "health = $4, max_health = $5, mana = $6, max_mana = $7, "
        "experience = $8 "
        "WHERE character_id = $9",
        9,
        NULL,
        param_values,
        NULL, NULL, 0
    );

    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        LOG_ERROR("Failed to update character: %s", PQerrorMessage(conn));
        PQclear(res);
        return 0;
    }

    /* Zero rows updated means the character is gone from this database -- it
     * was deleted while it was live in a world. Reporting that as a successful
     * save is what let a deleted character keep "saving" for the rest of its
     * session, and what let the item writes below re-INSERT rows for a
     * character that no longer exists. */
    if (atoi(PQcmdTuples(res)) == 0) {
        LOG_ERROR("Character %u no longer exists; refusing to save it",
                  char_info->character_id);
        PQclear(res);
        return 0;
    }
    PQclear(res);

    /* Balances commit in the same transaction as the scalars, so a crash
     * between the two can never bank coin the player did not keep. */
    for (int c = 0; c < CURRENCY_COUNT; c++) {
        char currency_id_str[16], amount_str[32];
        snprintf(currency_id_str, sizeof(currency_id_str), "%d", c);
        snprintf(amount_str, sizeof(amount_str), "%u", char_info->currency[c]);

        const char* currency_params[3] = {
            character_id_str,   // $1
            currency_id_str,    // $2
            amount_str          // $3
        };

        res = PQexecParams(conn,
            "INSERT INTO character_currencies (character_id, currency_id, amount) "
            "VALUES ($1, $2, $3) "
            "ON CONFLICT (character_id, currency_id) DO UPDATE SET amount = EXCLUDED.amount",
            3, NULL, currency_params, NULL, NULL, 0);

        if (PQresultStatus(res) != PGRES_COMMAND_OK) {
            LOG_ERROR("Failed to update currency %d for character %u: %s",
                      c, char_info->character_id, PQerrorMessage(conn));
            PQclear(res);
            return 0;
        }
        PQclear(res);
    }

    return 1;
}

/**
 * Commit a character's scalar state in one transaction.
 *
 * @return 1 after commit, or 0 for NULL input or database failure.
 */
int character_update_full_data(const CharacterInfo* char_info) {
    if (!char_info) {
        LOG_ERROR("character_update_full_data: NULL char_info pointer");
        return 0;
    }

    PGconn* conn = acquire_connection();
    if (!conn) {
        LOG_ERROR("Failed to acquire connection for character update");
        return 0;
    }

    if (!tx_exec(conn, "BEGIN")) {
        release_connection(conn);
        return 0;
    }

    if (!update_scalars_tx(conn, char_info)) {
        tx_exec(conn, "ROLLBACK");
        release_connection(conn);
        return 0;
    }

    int committed = tx_exec(conn, "COMMIT");
    if (!committed) tx_exec(conn, "ROLLBACK");
    release_connection(conn);
    return committed;
}

/**
 * Find the highest persisted item-instance identifier, reporting query success.
 *
 * The value and the success flag are separate on purpose. A failed query and an
 * empty table both yield 0, and the item-instance allocator cannot tell them
 * apart: seeding from a failed query restarts identifier issuance at 1 and
 * reissues identifiers that persisted rows already own, which collides on
 * character_items' primary key and silently rewrites other characters' items.
 * Startup must be able to refuse to run rather than seed from a guess.
 *
 * @param out_highest  Receives the highest identifier; untouched on failure.
 * @return             1 when the query succeeded, otherwise 0.
 */
int character_items_max_instance_id_checked(uint64_t* out_highest) {
    if (!out_highest) return 0;

    PGconn* conn = acquire_connection();
    if (!conn) {
        LOG_ERROR("character_items_max_instance_id: no database connection");
        return 0;
    }

    PGresult* res = PQexec(conn, "SELECT COALESCE(MAX(instance_id), 0) FROM character_items");
    int ok = 0;
    if (PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) == 1) {
        *out_highest = strtoull(PQgetvalue(res, 0, 0), NULL, 10);
        ok = 1;
    } else {
        LOG_ERROR("character_items_max_instance_id: %s", PQerrorMessage(conn));
    }

    PQclear(res);
    release_connection(conn);
    return ok;
}

/**
 * Find the highest persisted item-instance identifier.
 *
 * @return The highest identifier, or 0 for an empty table or query failure.
 */
uint64_t character_items_max_instance_id(void) {
    uint64_t highest = 0;
    (void)character_items_max_instance_id_checked(&highest);
    return highest;
}

/**
 * Load and fully replace a character's inventory and equipment arrays.
 *
 * @param inventory       Output inventory array.
 * @param inventory_slots Inventory capacity.
 * @param equipment       Output equipment array.
 * @param equip_slots     Equipment capacity.
 * @return                1 on success, or 0 on invalid input or query failure.
 */
int character_items_load(uint32_t character_id,
                         ItemInstance* inventory, int inventory_slots,
                         ItemInstance* equipment, int equip_slots) {
    if (!inventory || !equipment) return 0;

    // Absent rows mean empty slots, so start from clear rather than trusting
    // the caller's buffer.
    memset(inventory, 0, (size_t)inventory_slots * sizeof(*inventory));
    memset(equipment, 0, (size_t)equip_slots * sizeof(*equipment));

    PGconn* conn = acquire_connection();
    if (!conn) return 0;

    char id_str[32];
    snprintf(id_str, sizeof(id_str), "%u", character_id);
    const char* params[1] = { id_str };

    PGresult* res = PQexecParams(conn,
        "SELECT instance_id, slot, item_id, quantity, is_bound "
        "FROM character_items WHERE character_id = $1",
        1, NULL, params, NULL, NULL, 0);

    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        LOG_ERROR("character_items_load(%u): %s", character_id, PQerrorMessage(conn));
        PQclear(res);
        release_connection(conn);
        return 0;
    }

    int rows = PQntuples(res);
    for (int r = 0; r < rows; r++) {
        ItemInstance item;
        item.instance_id = strtoull(PQgetvalue(res, r, 0), NULL, 10);
        int slot         = atoi(PQgetvalue(res, r, 1));
        item.item_id     = (uint32_t)strtoul(PQgetvalue(res, r, 2), NULL, 10);
        item.quantity    = (uint16_t)atoi(PQgetvalue(res, r, 3));
        item.is_bound    = (PQgetvalue(res, r, 4)[0] == 't') ? 1 : 0;
        item._pad        = 0;

        if (item.instance_id == 0 || item.quantity == 0) continue;

        // ignore persisted slots outside both destination arrays
        if (slot >= 0 && slot < inventory_slots) {
            inventory[slot] = item;
        } else if (slot >= EQUIP_SLOT_BASE && slot < EQUIP_SLOT_BASE + equip_slots) {
            equipment[slot - EQUIP_SLOT_BASE] = item;
        } else {
            LOG_ERROR("character_items_load(%u): ignoring row in slot %d",
                      character_id, slot);
        }
    }

    PQclear(res);
    release_connection(conn);
    return 1;
}

/* --- Building array literals for the item save ---------------------------
 *
 * The save below sends five parallel PostgreSQL array literals instead of one
 * statement per occupied slot. These three functions are the whole of the
 * machinery: append a formatted element, notice if it did not fit, and close
 * the brace. Overflow is sticky and checked once at the end rather than at
 * every call site, because the only correct response to any of them is the
 * same -- abandon the save rather than write a partial one.
 */
typedef struct {
    char*  buf;
    size_t cap;
    size_t used;      /**< Bytes written, excluding the terminator. */
    int    count;     /**< Elements appended. */
    int    overflow;  /**< Set once anything failed to fit. */
} ArrayBuf;

static int arraybuf_init(ArrayBuf* b, size_t cap) {
    b->buf = malloc(cap);
    if (!b->buf) return 0;
    b->cap = cap;
    b->used = 0;
    b->count = 0;
    b->overflow = 0;
    b->buf[b->used++] = '{';
    b->buf[b->used] = '\0';
    return 1;
}

static void arraybuf_append(ArrayBuf* b, const char* fmt, ...) {
    if (b->overflow) return;

    /* Room for the element, its separator, the closing brace and the
     * terminator. Reserving the last two here is what lets arraybuf_close()
     * be infallible. */
    size_t room = b->cap - b->used - 2;
    const char* sep = (b->count > 0) ? "," : "";

    va_list ap;
    va_start(ap, fmt);
    int w = vsnprintf(b->buf + b->used, room, fmt, ap);
    va_end(ap);

    size_t sep_len = strlen(sep);
    if (w < 0 || (size_t)w + sep_len >= room) { b->overflow = 1; return; }

    if (sep_len) {
        /* Shift the element right by one and write the comma in front of it,
         * rather than formatting the separator into the format string at
         * every call site. */
        memmove(b->buf + b->used + sep_len, b->buf + b->used, (size_t)w + 1);
        b->buf[b->used] = ',';
    }
    b->used += sep_len + (size_t)w;
    b->count++;
}

static int arraybuf_close(ArrayBuf* b) {
    if (b->overflow) return 0;
    b->buf[b->used++] = '}';
    b->buf[b->used] = '\0';
    return 1;
}

static void arraybuf_free(ArrayBuf* b) { free(b->buf); b->buf = NULL; }

/**
 * Replace a character's persisted item set inside an open transaction.
 *
 * The caller owns the transaction. Split out of character_items_save() so the
 * same statements can run inside a transaction that also carries the scalar
 * and currency writes -- see character_save_all().
 *
 * Two statements, whatever the character is carrying: one DELETE for the
 * instances that are gone, and one INSERT ... ON CONFLICT for the ones that
 * remain. It used to be one round trip per occupied slot -- up to 158 of them,
 * each a full request/response against PostgreSQL, on the path that runs every
 * time a player is saved. The rows are passed as five parallel array
 * parameters and expanded server-side by unnest(), so the statement text is
 * fixed and the parameter count does not grow with the inventory.
 *
 * @return 1 on success, or 0 on allocation or database failure.
 */
static int save_items_tx(PGconn* conn, uint32_t character_id,
                         const ItemInstance* inventory, int inventory_slots,
                         const ItemInstance* equipment, int equip_slots) {
    const int total_slots = inventory_slots + equip_slots;
    if (total_slots < 0) return 0;

    /* Every buffer is sized from the widest value its element type can print,
     * never from an estimate. An identifier that did not fit used to be
     * silently skipped, after which the DELETE removed exactly the items that
     * were skipped -- so the failure mode of a too-small buffer here is silent
     * item loss, and it is worth a few unused bytes to make it unreachable. */
    const size_t per_id   = 21;  /* 20 digits of uint64_t, plus a separator  */
    const size_t per_slot = 8;   /* -32768 plus a separator                  */
    const size_t per_item = 12;  /* 10 digits of uint32_t, plus a separator  */
    const size_t per_qty  = 8;
    const size_t per_bool = 2;   /* 't' or 'f', plus a separator             */

    ArrayBuf ids = {0}, slots = {0}, items = {0}, qtys = {0}, bounds = {0};
    uint64_t* seen = total_slots > 0 ? malloc(sizeof(uint64_t) * (size_t)total_slots) : NULL;
    int seen_count = 0;

    int built =
        arraybuf_init(&ids,    (size_t)total_slots * per_id   + 8) &&
        arraybuf_init(&slots,  (size_t)total_slots * per_slot + 8) &&
        arraybuf_init(&items,  (size_t)total_slots * per_item + 8) &&
        arraybuf_init(&qtys,   (size_t)total_slots * per_qty  + 8) &&
        arraybuf_init(&bounds, (size_t)total_slots * per_bool + 8) &&
        (total_slots == 0 || seen != NULL);

    if (!built) {
        LOG_ERROR("character_items_save(%u): out of memory building the item save",
                  character_id);
        goto fail;
    }

    for (int pass = 0; pass < 2; pass++) {
        const ItemInstance* arr = (pass == 0) ? inventory : equipment;
        int   n                 = (pass == 0) ? inventory_slots : equip_slots;
        int   slot_base         = (pass == 0) ? 0 : EQUIP_SLOT_BASE;

        for (int i = 0; i < n; i++) {
            if (arr[i].instance_id == 0 || arr[i].quantity == 0) continue;

            /* One instance in two slots at once is item duplication, and it
             * must not be persisted as if it were two items. The per-row loop
             * this replaced would have written the second row over the first
             * and reported success; a single statement cannot, and this says
             * so plainly rather than letting PostgreSQL report "cannot affect
             * row a second time" from inside a save. */
            for (int j = 0; j < seen_count; j++) {
                if (seen[j] == arr[i].instance_id) {
                    LOG_ERROR("character_items_save(%u): instance %llu appears in "
                              "two slots at once — refusing to save",
                              character_id, (unsigned long long)arr[i].instance_id);
                    goto fail;
                }
            }
            seen[seen_count++] = arr[i].instance_id;

            arraybuf_append(&ids,    "%llu", (unsigned long long)arr[i].instance_id);
            arraybuf_append(&slots,  "%d",   slot_base + i);
            arraybuf_append(&items,  "%u",   arr[i].item_id);
            arraybuf_append(&qtys,   "%u",   arr[i].quantity);
            arraybuf_append(&bounds, "%s",   arr[i].is_bound ? "t" : "f");
        }
    }

    if (!arraybuf_close(&ids)    || !arraybuf_close(&slots) ||
        !arraybuf_close(&items)  || !arraybuf_close(&qtys)  ||
        !arraybuf_close(&bounds)) {
        LOG_ERROR("character_items_save(%u): the item list did not fit its buffers; "
                  "refusing to delete items", character_id);
        goto fail;
    }

    char id_str[32];
    snprintf(id_str, sizeof(id_str), "%u", character_id);

    /* Delete what the character is no longer carrying. Everything still held
     * is named in `ids`, so this is "delete the rest" rather than a list of
     * things to remove -- an item that vanished for any reason at all is
     * covered without anything having to notice how it vanished. */
    const char* del_params[2] = { id_str, ids.buf };
    PGresult* res = PQexecParams(conn,
        "DELETE FROM character_items "
        "WHERE character_id = $1 AND NOT (instance_id = ANY($2::bigint[]))",
        2, NULL, del_params, NULL, NULL, 0);

    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        LOG_ERROR("character_items_save(%u): delete failed: %s",
                  character_id, PQerrorMessage(conn));
        PQclear(res);
        goto fail;
    }
    PQclear(res);

    /* Nothing retained: the delete above emptied the character and there is
     * no upsert to run. Sending an empty unnest() would be harmless but it is
     * a round trip that does nothing. */
    if (ids.count == 0) {
        arraybuf_free(&ids); arraybuf_free(&slots); arraybuf_free(&items);
        arraybuf_free(&qtys); arraybuf_free(&bounds); free(seen);
        return 1;
    }

    /* Upsert the retained stacks without replacing their identity. ON CONFLICT
     * targets instance_id, so a stack that only moved or changed size keeps
     * the identifier it was issued -- which is what makes an item the same
     * item across a save. */
    const char* upsert =
        "INSERT INTO character_items "
        "  (instance_id, character_id, slot, item_id, quantity, is_bound) "
        "SELECT u.instance_id, $1::integer, u.slot, u.item_id, u.quantity, u.is_bound "
        "FROM unnest($2::bigint[], $3::smallint[], $4::integer[], "
        "            $5::smallint[], $6::boolean[]) "
        "     AS u(instance_id, slot, item_id, quantity, is_bound) "
        "ON CONFLICT (instance_id) DO UPDATE SET "
        "  slot = EXCLUDED.slot, quantity = EXCLUDED.quantity, "
        "  is_bound = EXCLUDED.is_bound";

    const char* up_params[6] = { id_str, ids.buf, slots.buf, items.buf,
                                 qtys.buf, bounds.buf };

    res = PQexecParams(conn, upsert, 6, NULL, up_params, NULL, NULL, 0);
    int ok = (PQresultStatus(res) == PGRES_COMMAND_OK);
    if (!ok)
        LOG_ERROR("character_items_save(%u): upsert of %d items failed: %s",
                  character_id, ids.count, PQerrorMessage(conn));
    PQclear(res);

    arraybuf_free(&ids); arraybuf_free(&slots); arraybuf_free(&items);
    arraybuf_free(&qtys); arraybuf_free(&bounds); free(seen);
    return ok;

fail:
    arraybuf_free(&ids); arraybuf_free(&slots); arraybuf_free(&items);
    arraybuf_free(&qtys); arraybuf_free(&bounds); free(seen);
    return 0;
}

/**
 * Replace a character's persisted item set in one transaction.
 *
 * Preserves identifiers for retained instances and deletes instances absent from both arrays.
 *
 * @return 1 after commit, or 0 on invalid input, allocation, or database failure.
 */
int character_items_save(uint32_t character_id,
                         const ItemInstance* inventory, int inventory_slots,
                         const ItemInstance* equipment, int equip_slots) {
    if (!inventory || !equipment) return 0;

    PGconn* conn = acquire_connection();
    if (!conn) return 0;

    if (!tx_exec(conn, "BEGIN")) {
        release_connection(conn);
        return 0;
    }

    if (!save_items_tx(conn, character_id, inventory, inventory_slots,
                       equipment, equip_slots)) {
        tx_exec(conn, "ROLLBACK");
        release_connection(conn);
        return 0;
    }

    int committed = tx_exec(conn, "COMMIT");
    if (!committed) tx_exec(conn, "ROLLBACK");
    release_connection(conn);
    return committed;
}

/**
 * Commit a character's scalars, currency balances and items in one transaction.
 *
 * These used to be two independent transactions. A failure between them -- a
 * lost connection, a crash, a constraint violation on the second -- persisted
 * currency without the items it bought, or items without the currency they
 * cost, and nothing afterwards could tell that had happened. One transaction
 * makes the pair atomic: either the whole save lands or none of it does.
 *
 * @return 1 after commit, or 0 on invalid input or database failure.
 */
int character_save_all(const CharacterInfo* char_info,
                       const ItemInstance* inventory, int inventory_slots,
                       const ItemInstance* equipment, int equip_slots) {
    if (!char_info || !inventory || !equipment) return 0;

    PGconn* conn = acquire_connection();
    if (!conn) {
        LOG_ERROR("Failed to acquire connection for character %u",
                  char_info->character_id);
        return 0;
    }

    if (!tx_exec(conn, "BEGIN")) {
        release_connection(conn);
        return 0;
    }

    if (!update_scalars_tx(conn, char_info) ||
        !save_items_tx(conn, char_info->character_id,
                       inventory, inventory_slots, equipment, equip_slots)) {
        tx_exec(conn, "ROLLBACK");
        release_connection(conn);
        return 0;
    }

    int committed = tx_exec(conn, "COMMIT");
    if (!committed) tx_exec(conn, "ROLLBACK");
    release_connection(conn);
    return committed;
}

/**
 * Retrieve the account that owns a character.
 *
 * @return The account identifier, or 0 when absent or on query failure.
 */
uint32_t character_get_owner(uint32_t character_id) {
    PGconn* conn = acquire_connection();
    if (!conn) {
        LOG_ERROR("Failed to acquire connection for owner check");
        return 0;
    }

    char character_id_str[32];
    snprintf(character_id_str, sizeof(character_id_str), "%u", character_id);

    const char* param_values[1] = {character_id_str};

    PGresult* res = PQexecParams(conn,
        "SELECT account_id FROM characters WHERE character_id = $1",
        1,
        NULL,
        param_values,
        NULL, NULL, 0
    );

    if (PQresultStatus(res) != PGRES_TUPLES_OK || PQntuples(res) == 0) {
        if (PQresultStatus(res) == PGRES_TUPLES_OK) {
            LOG_ERROR("Character %u not found in database", character_id);
        } else {
            LOG_ERROR("Database error getting character owner: %s",
                      PQerrorMessage(conn));
        }
        PQclear(res);
        release_connection(conn);
        return 0;
    }

    uint32_t account_id = (uint32_t)strtoul(PQgetvalue(res, 0, 0), NULL, 10);

    PQclear(res);
    release_connection(conn);

    return account_id;
}
