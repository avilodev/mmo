/**
 * @file
 * Manage pooled PostgreSQL access for character records and persistent item instances.
 */

#include "players_database.h"
#include "class_stats.h"
#include "world_regions.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>

#define CONN_POOL_SIZE 4
#define CONN_ACQUIRE_TIMEOUT_MS 5000

/** Track one PostgreSQL connection and its checkout state. */
typedef struct {
    PGconn* conn;
    int in_use;
    pthread_mutex_t lock;
} PooledConnection;

/** Own the fixed connection pool and its availability synchronization. */
typedef struct {
    PooledConnection connections[CONN_POOL_SIZE];
    char connection_string[512];
    int initialized;
    pthread_mutex_t pool_lock;
    pthread_cond_t conn_available;
} ConnectionPool;

static ConnectionPool g_pool = {0};

static PGconn* acquire_connection(void);
static void release_connection(PGconn* conn);
static int reconnect_if_needed(PooledConnection* pc);

/**
 * Initialize the PostgreSQL pool and required character tables and indexes.
 *
 * Performs blocking connection and schema operations.
 *
 * @return 1 when initialized or already active, or 0 on connection or required-schema failure.
 */
int character_database_init(const char* connection_string) {
    if (g_pool.initialized) {
        fprintf(stderr, "Database already initialized\n");
        return 1;
    }

    pthread_mutex_init(&g_pool.pool_lock, NULL);
    pthread_cond_init(&g_pool.conn_available, NULL);

    strncpy(g_pool.connection_string, connection_string, sizeof(g_pool.connection_string) - 1);
    g_pool.connection_string[sizeof(g_pool.connection_string) - 1] = '\0';

    // Initialize connection pool
    for (int i = 0; i < CONN_POOL_SIZE; i++) {
        pthread_mutex_init(&g_pool.connections[i].lock, NULL);
        g_pool.connections[i].in_use = 0;
        g_pool.connections[i].conn = PQconnectdb(connection_string);

        if (PQstatus(g_pool.connections[i].conn) != CONNECTION_OK) {
            fprintf(stderr, "PostgreSQL connection %d failed: %s\n",
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
            return 0;
        }

        // Set connection to non-blocking mode for better timeout handling
        PQsetnonblocking(g_pool.connections[i].conn, 0);
    }

    g_pool.initialized = 1;

    // Create tables and indexes using one connection
    PGconn* setup_conn = acquire_connection();
    if (!setup_conn) {
        fprintf(stderr, "Failed to acquire connection for setup\n");
        character_database_close();
        return 0;
    }

    // Create characters table if it doesn't exist (WITH EQUIPMENT FIELDS)
    const char* create_table =
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
        ");";

    PGresult* res = PQexec(setup_conn, create_table);
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        fprintf(stderr, "Failed to create characters table: %s\n",
                PQerrorMessage(setup_conn));
        PQclear(res);
        release_connection(setup_conn);
        character_database_close();
        return 0;
    }
    PQclear(res);

    // drop legacy item columns superseded by character_items
    const char* drop_item_columns =
        "ALTER TABLE characters "
        "  DROP COLUMN IF EXISTS helmet,"
        "  DROP COLUMN IF EXISTS gloves,"
        "  DROP COLUMN IF EXISTS chest_armor,"
        "  DROP COLUMN IF EXISTS leggings,"
        "  DROP COLUMN IF EXISTS boots,"
        "  DROP COLUMN IF EXISTS main_hand,"
        "  DROP COLUMN IF EXISTS second_hand,"
        "  DROP COLUMN IF EXISTS effect,"
        "  DROP COLUMN IF EXISTS inventory;";

    res = PQexec(setup_conn, drop_item_columns);
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        fprintf(stderr, "Warning: failed to drop legacy item columns: %s\n",
                PQerrorMessage(setup_conn));
    }
    PQclear(res);

    // Add mana columns (migration)
    const char* add_mana_columns =
        "DO $$ BEGIN "
        "ALTER TABLE characters ADD COLUMN IF NOT EXISTS mana INTEGER DEFAULT 100; "
        "ALTER TABLE characters ADD COLUMN IF NOT EXISTS max_mana INTEGER DEFAULT 100; "
        "END $$;";

    res = PQexec(setup_conn, add_mana_columns);
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        fprintf(stderr, "Warning: Failed to add mana columns: %s\n",
                PQerrorMessage(setup_conn));
    }
    PQclear(res);

    // reserve nullable durability and rolled-stat columns for item instances
    const char* create_items_table =
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
        // enforce one item instance per character slot
        "    UNIQUE (character_id, slot)"
        ");";

    res = PQexec(setup_conn, create_items_table);
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        fprintf(stderr, "Failed to create character_items table: %s\n",
                PQerrorMessage(setup_conn));
        PQclear(res);
        release_connection(setup_conn);
        character_database_close();
        return 0;
    }
    PQclear(res);

    /* One row per character per kingdom currency. A table rather than a column
     * per currency so adding a kingdom needs no schema change — the row's
     * currency_id is a CurrencyId from the shared city table. */
    const char* create_currency_table =
        "CREATE TABLE IF NOT EXISTS character_currencies ("
        "    character_id INTEGER NOT NULL,"
        "    currency_id  SMALLINT NOT NULL,"
        "    amount       BIGINT NOT NULL DEFAULT 0 CHECK (amount >= 0),"
        "    PRIMARY KEY (character_id, currency_id)"
        ");";

    res = PQexec(setup_conn, create_currency_table);
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        fprintf(stderr, "Failed to create character_currencies table: %s\n",
                PQerrorMessage(setup_conn));
        PQclear(res);
        release_connection(setup_conn);
        character_database_close();
        return 0;
    }
    PQclear(res);

    /* Seed Ennara balances from the legacy single-gold column, once. Existing
     * rows win, so this is safe to re-run and cannot overwrite live balances.
     * The `gold` column is deliberately left in place, unread and unwritten,
     * so a rollback to a pre-currency build still finds its data. */
    char seed_sql[256];
    snprintf(seed_sql, sizeof(seed_sql),
             "INSERT INTO character_currencies (character_id, currency_id, amount) "
             "SELECT character_id, %d, gold FROM characters WHERE gold > 0 "
             "ON CONFLICT (character_id, currency_id) DO NOTHING;",
             (int)CURRENCY_ENNARA);
    res = PQexec(setup_conn, seed_sql);
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        fprintf(stderr, "Warning: failed to seed currencies from legacy gold: %s\n",
                PQerrorMessage(setup_conn));
    }
    PQclear(res);

    // index item ownership for character loading
    res = PQexec(setup_conn,
        "CREATE INDEX IF NOT EXISTS idx_character_items_owner "
        "ON character_items(character_id);");
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        fprintf(stderr, "Warning: failed to index character_items: %s\n",
                PQerrorMessage(setup_conn));
    }
    PQclear(res);

    // Create index on account_id and world_id for faster lookups
    const char* create_index =
        "CREATE INDEX IF NOT EXISTS idx_characters_account_world "
        "ON characters(account_id, world_id);";

    res = PQexec(setup_conn, create_index);
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        fprintf(stderr, "Warning: Failed to create index: %s\n",
                PQerrorMessage(setup_conn));
    }
    PQclear(res);

    release_connection(setup_conn);

    printf("PostgreSQL character database initialized successfully with %d connections\n",
           CONN_POOL_SIZE);
    return 1;
}

/**
 * Close every pooled connection and destroy pool synchronization objects.
 */
void character_database_close(void) {
    if (!g_pool.initialized) {
        return;
    }

    pthread_mutex_lock(&g_pool.pool_lock);

    for (int i = 0; i < CONN_POOL_SIZE; i++) {
        pthread_mutex_lock(&g_pool.connections[i].lock);
        if (g_pool.connections[i].conn) {
            PQfinish(g_pool.connections[i].conn);
            g_pool.connections[i].conn = NULL;
        }
        pthread_mutex_unlock(&g_pool.connections[i].lock);
        pthread_mutex_destroy(&g_pool.connections[i].lock);
    }

    g_pool.initialized = 0;
    pthread_mutex_unlock(&g_pool.pool_lock);

    pthread_mutex_destroy(&g_pool.pool_lock);
    pthread_cond_destroy(&g_pool.conn_available);

    printf("PostgreSQL connection pool closed\n");
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

    fprintf(stderr, "Connection lost, attempting to reconnect...\n");
    PQreset(pc->conn);

    if (PQstatus(pc->conn) == CONNECTION_OK) {
        printf("Successfully reconnected\n");
        return 1;
    }

    fprintf(stderr, "Reconnection failed: %s\n", PQerrorMessage(pc->conn));
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
        // Try to find an available connection
        for (int i = 0; i < CONN_POOL_SIZE; i++) {
            if (!g_pool.connections[i].in_use) {
                pthread_mutex_lock(&g_pool.connections[i].lock);

                // Double-check after acquiring lock
                if (!g_pool.connections[i].in_use) {
                    g_pool.connections[i].in_use = 1;

                    // Verify connection is good
                    if (!reconnect_if_needed(&g_pool.connections[i])) {
                        g_pool.connections[i].in_use = 0;
                        pthread_mutex_unlock(&g_pool.connections[i].lock);
                        pthread_mutex_unlock(&g_pool.pool_lock);
                        return NULL;
                    }

                    pthread_mutex_unlock(&g_pool.connections[i].lock);
                    pthread_mutex_unlock(&g_pool.pool_lock);
                    return g_pool.connections[i].conn;
                }

                pthread_mutex_unlock(&g_pool.connections[i].lock);
            }
        }

        // No connections available, wait with timeout
        int wait_result = pthread_cond_timedwait(&g_pool.conn_available,
                                                  &g_pool.pool_lock, &timeout);

        if (wait_result == ETIMEDOUT) {
            pthread_mutex_unlock(&g_pool.pool_lock);
            fprintf(stderr, "Timeout waiting for database connection\n");
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

    for (int i = 0; i < CONN_POOL_SIZE; i++) {
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
        fprintf(stderr, "Failed to acquire connection for character list\n");
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
        fprintf(stderr, "Failed to get character list: %s\n", PQerrorMessage(conn));
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
        fprintf(stderr, "Failed to acquire connection for character count\n");
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
        fprintf(stderr, "Failed to acquire connection for character creation\n");
        return 0;
    }

    // Begin transaction for atomicity
    PGresult* res = PQexec(conn, "BEGIN");
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        fprintf(stderr, "Failed to begin transaction: %s\n", PQerrorMessage(conn));
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
        fprintf(stderr, "Refusing to create '%s': race %d is not in the registry\n",
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
        fprintf(stderr, "Failed to create character: %s\n", PQerrorMessage(conn));
        PQclear(res);
        PQexec(conn, "ROLLBACK");
        release_connection(conn);
        return 0;
    }

    if (PQntuples(res) == 0) {
        fprintf(stderr, "Character creation returned no ID\n");
        PQclear(res);
        PQexec(conn, "ROLLBACK");
        release_connection(conn);
        return 0;
    }

    *out_character_id = (uint32_t)strtoul(PQgetvalue(res, 0, 0), NULL, 10);
    PQclear(res);

    // Commit transaction
    res = PQexec(conn, "COMMIT");
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        fprintf(stderr, "Failed to commit transaction: %s\n", PQerrorMessage(conn));
        PQclear(res);
        PQexec(conn, "ROLLBACK");
        release_connection(conn);
        return 0;
    }
    PQclear(res);

    printf("Successfully created character '%s' (race %d) with ID %u for account %u in world %u (hp=%d, resource=%d)\n",
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
        fprintf(stderr, "Failed to acquire connection for ownership check\n");
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
        fprintf(stderr, "character_get_full_data: NULL char_info pointer\n");
        return 0;
    }

    PGconn* conn = acquire_connection();
    if (!conn) {
        fprintf(stderr, "Failed to acquire connection for full character data\n");
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
        fprintf(stderr, "Failed to get full character data: %s\n", PQerrorMessage(conn));
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
        fprintf(stderr, "Failed to load currencies for character %u: %s\n",
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
            fprintf(stderr, "character %u holds unknown currency %d — ignoring\n",
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

/**
 * Commit a character's scalar state in one transaction.
 *
 * @return 1 after commit, or 0 for NULL input or database failure.
 */
int character_update_full_data(const CharacterInfo* char_info) {
    if (!char_info) {
        fprintf(stderr, "character_update_full_data: NULL char_info pointer\n");
        return 0;
    }

    PGconn* conn = acquire_connection();
    if (!conn) {
        fprintf(stderr, "Failed to acquire connection for character update\n");
        return 0;
    }

    // Begin transaction
    PGresult* res = PQexec(conn, "BEGIN");
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        fprintf(stderr, "Failed to begin transaction: %s\n", PQerrorMessage(conn));
        PQclear(res);
        release_connection(conn);
        return 0;
    }
    PQclear(res);

    // item instances persist through character_items_save
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

    res = PQexecParams(conn,
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
        fprintf(stderr, "Failed to update character: %s\n", PQerrorMessage(conn));
        PQclear(res);
        PQexec(conn, "ROLLBACK");
        release_connection(conn);
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
            fprintf(stderr, "Failed to update currency %d for character %u: %s\n",
                    c, char_info->character_id, PQerrorMessage(conn));
            PQclear(res);
            PQexec(conn, "ROLLBACK");
            release_connection(conn);
            return 0;
        }
        PQclear(res);
    }

    // Commit transaction
    res = PQexec(conn, "COMMIT");
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        fprintf(stderr, "Failed to commit transaction: %s\n", PQerrorMessage(conn));
        PQclear(res);
        release_connection(conn);
        return 0;
    }
    PQclear(res);

    release_connection(conn);
    return 1;
}

/**
 * Find the highest persisted item-instance identifier.
 *
 * @return The highest identifier, or 0 for an empty table or query failure.
 */
uint64_t character_items_max_instance_id(void) {
    PGconn* conn = acquire_connection();
    if (!conn) return 0;

    PGresult* res = PQexec(conn, "SELECT COALESCE(MAX(instance_id), 0) FROM character_items");
    uint64_t highest = 0;
    if (PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) == 1)
        highest = strtoull(PQgetvalue(res, 0, 0), NULL, 10);
    else
        fprintf(stderr, "character_items_max_instance_id: %s\n", PQerrorMessage(conn));

    PQclear(res);
    release_connection(conn);
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
        fprintf(stderr, "character_items_load(%u): %s\n", character_id, PQerrorMessage(conn));
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
            fprintf(stderr, "character_items_load(%u): ignoring row in slot %d\n",
                    character_id, slot);
        }
    }

    PQclear(res);
    release_connection(conn);
    return 1;
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

    PGresult* res = PQexec(conn, "BEGIN");
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        fprintf(stderr, "character_items_save: BEGIN failed: %s\n", PQerrorMessage(conn));
        PQclear(res);
        release_connection(conn);
        return 0;
    }
    PQclear(res);

    // retain identifiers present in either current item array
    size_t keep_cap = (size_t)(inventory_slots + equip_slots) * 24 + 32;
    char*  keep = malloc(keep_cap);
    if (!keep) {
        PQexec(conn, "ROLLBACK");
        release_connection(conn);
        return 0;
    }

    size_t used = 0;
    keep[used++] = '{';

    #define APPEND_KEEP(inst)                                                     \
        do {                                                                      \
            if ((inst).instance_id != 0 && (inst).quantity != 0) {                \
                int w = snprintf(keep + used, keep_cap - used, "%s%llu",           \
                                 used > 1 ? "," : "",                              \
                                 (unsigned long long)(inst).instance_id);          \
                if (w > 0 && (size_t)w < keep_cap - used) used += (size_t)w;        \
            }                                                                     \
        } while (0)

    for (int i = 0; i < inventory_slots; i++) APPEND_KEEP(inventory[i]);
    for (int i = 0; i < equip_slots; i++)     APPEND_KEEP(equipment[i]);
    #undef APPEND_KEEP

    if (used + 2 >= keep_cap) { free(keep); PQexec(conn, "ROLLBACK"); release_connection(conn); return 0; }
    keep[used++] = '}';
    keep[used]   = '\0';

    char id_str[32];
    snprintf(id_str, sizeof(id_str), "%u", character_id);
    const char* del_params[2] = { id_str, keep };

    res = PQexecParams(conn,
        "DELETE FROM character_items "
        "WHERE character_id = $1 AND NOT (instance_id = ANY($2::bigint[]))",
        2, NULL, del_params, NULL, NULL, 0);

    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        fprintf(stderr, "character_items_save(%u): delete failed: %s\n",
                character_id, PQerrorMessage(conn));
        PQclear(res);
        free(keep);
        PQexec(conn, "ROLLBACK");
        release_connection(conn);
        return 0;
    }
    PQclear(res);
    free(keep);

    // upsert retained stacks without replacing their identity
    const char* upsert =
        "INSERT INTO character_items "
        "  (instance_id, character_id, slot, item_id, quantity, is_bound) "
        "VALUES ($1, $2, $3, $4, $5, $6) "
        "ON CONFLICT (instance_id) DO UPDATE SET "
        "  slot = EXCLUDED.slot, quantity = EXCLUDED.quantity, "
        "  is_bound = EXCLUDED.is_bound";

    int ok = 1;
    for (int pass = 0; pass < 2 && ok; pass++) {
        const ItemInstance* arr = pass == 0 ? inventory : equipment;
        int   n                 = pass == 0 ? inventory_slots : equip_slots;
        int   slot_base         = pass == 0 ? 0 : EQUIP_SLOT_BASE;

        for (int i = 0; i < n; i++) {
            if (arr[i].instance_id == 0 || arr[i].quantity == 0) continue;

            char inst_str[32], slot_str[16], item_str[32], qty_str[16];
            snprintf(inst_str, sizeof(inst_str), "%llu",
                     (unsigned long long)arr[i].instance_id);
            snprintf(slot_str, sizeof(slot_str), "%d", slot_base + i);
            snprintf(item_str, sizeof(item_str), "%u", arr[i].item_id);
            snprintf(qty_str,  sizeof(qty_str),  "%u", arr[i].quantity);

            const char* p[6] = { inst_str, id_str, slot_str, item_str, qty_str,
                                 arr[i].is_bound ? "true" : "false" };

            res = PQexecParams(conn, upsert, 6, NULL, p, NULL, NULL, 0);
            if (PQresultStatus(res) != PGRES_COMMAND_OK) {
                fprintf(stderr, "character_items_save(%u): upsert slot %d failed: %s\n",
                        character_id, slot_base + i, PQerrorMessage(conn));
                ok = 0;
            }
            PQclear(res);
            if (!ok) break;
        }
    }

    if (!ok) {
        PQexec(conn, "ROLLBACK");
        release_connection(conn);
        return 0;
    }

    res = PQexec(conn, "COMMIT");
    int committed = (PQresultStatus(res) == PGRES_COMMAND_OK);
    if (!committed)
        fprintf(stderr, "character_items_save(%u): COMMIT failed: %s\n",
                character_id, PQerrorMessage(conn));
    PQclear(res);

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
        fprintf(stderr, "Failed to acquire connection for owner check\n");
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
            fprintf(stderr, "Character %u not found in database\n", character_id);
        } else {
            fprintf(stderr, "Database error getting character owner: %s\n",
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
