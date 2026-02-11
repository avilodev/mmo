#include "players_database.h"
#include "class_stats.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>

// Connection pool configuration
#define CONN_POOL_SIZE 10 
#define CONN_ACQUIRE_TIMEOUT_MS 5000

typedef struct {
    PGconn* conn;
    int in_use;
    pthread_mutex_t lock;
} PooledConnection;

typedef struct {
    PooledConnection connections[CONN_POOL_SIZE];
    char connection_string[512];
    int initialized;
    pthread_mutex_t pool_lock;
    pthread_cond_t conn_available;
} ConnectionPool;

static ConnectionPool g_pool = {0};

// Forward declarations
static PGconn* acquire_connection(void);
static void release_connection(PGconn* conn);
static int reconnect_if_needed(PooledConnection* pc);

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
            
            // Clean up already created connections
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
        "    helmet INTEGER DEFAULT 0,"
        "    gloves INTEGER DEFAULT 0,"
        "    chest_armor INTEGER DEFAULT 0,"
        "    leggings INTEGER DEFAULT 0,"
        "    boots INTEGER DEFAULT 0,"
        "    main_hand INTEGER DEFAULT 0,"
        "    second_hand INTEGER DEFAULT 0,"
        "    effect SMALLINT DEFAULT 0,"
        "    inventory INTEGER[] DEFAULT ARRAY_FILL(0, ARRAY[150]),"
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
    
    // Check if equipment columns exist, if not add them (for migration)
    const char* add_equipment_columns = 
        "DO $$ BEGIN "
        "ALTER TABLE characters ADD COLUMN IF NOT EXISTS helmet INTEGER DEFAULT 0; "
        "ALTER TABLE characters ADD COLUMN IF NOT EXISTS gloves INTEGER DEFAULT 0; "
        "ALTER TABLE characters ADD COLUMN IF NOT EXISTS chest_armor INTEGER DEFAULT 0; "
        "ALTER TABLE characters ADD COLUMN IF NOT EXISTS leggings INTEGER DEFAULT 0; "
        "ALTER TABLE characters ADD COLUMN IF NOT EXISTS boots INTEGER DEFAULT 0; "
        "ALTER TABLE characters ADD COLUMN IF NOT EXISTS main_hand INTEGER DEFAULT 0; "
        "ALTER TABLE characters ADD COLUMN IF NOT EXISTS second_hand INTEGER DEFAULT 0; "
        "ALTER TABLE characters ADD COLUMN IF NOT EXISTS effect SMALLINT DEFAULT 0; "
        "ALTER TABLE characters ADD COLUMN IF NOT EXISTS inventory INTEGER[] DEFAULT ARRAY_FILL(0, ARRAY[150]); "
        "END $$;";
    
    res = PQexec(setup_conn, add_equipment_columns);
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        fprintf(stderr, "Warning: Failed to add equipment columns: %s\n", 
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
        characters[i].player_class = (uint32_t)strtoul(PQgetvalue(res, i, 3), NULL, 10);
        characters[i].player_race = (uint32_t)strtoul(PQgetvalue(res, i, 4), NULL, 10);
        characters[i].pos_x     = (float)strtod(PQgetvalue(res, i, 5), NULL);
        characters[i].pos_y     = (float)strtod(PQgetvalue(res, i, 6), NULL);
        characters[i].health    = (uint32_t)strtoul(PQgetvalue(res, i, 7), NULL, 10);
        characters[i].max_health = (uint32_t)strtoul(PQgetvalue(res, i, 8), NULL, 10);
    }
    
    PQclear(res);
    release_connection(conn);
    return count;
}

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
    
    // Compute initial stats for this class at level 1
    DerivedStats initial_stats;
    class_stats_compute((uint8_t)class_id, 1, &initial_stats);

    char health_str[32], max_health_str[32];
    char mana_str[32], max_mana_str[32];
    
    snprintf(health_str, sizeof(health_str), "%d", initial_stats.max_health);
    snprintf(max_health_str, sizeof(max_health_str), "%d", initial_stats.max_health);
    snprintf(mana_str, sizeof(mana_str), "%d", initial_stats.max_mana);
    snprintf(max_mana_str, sizeof(max_mana_str), "%d", initial_stats.max_mana);

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
    
    printf("Successfully created character '%s' (class %d) with ID %u for account %u in world %u (hp=%d, mana=%d)\n",
           name, class_id, *out_character_id, account_id, world_id, 
           initial_stats.max_health, initial_stats.max_mana);
    
    release_connection(conn);
    return 1;
}

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
        "       pos_x, pos_y, health, max_health, mana, max_mana, experience, gold, "
        "       helmet, gloves, "
        "       chest_armor, leggings,"
        "       boots, main_hand,"
        "       second_hand, effect, inventory "
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
    char_info->player_class = (uint32_t)strtoul(PQgetvalue(res, 0, 3), NULL, 10);
    char_info->player_race = (uint32_t)strtoul(PQgetvalue(res, 0, 4), NULL, 10);
    char_info->pos_x = (float)strtod(PQgetvalue(res, 0, 5), NULL);
    char_info->pos_y = (float)strtod(PQgetvalue(res, 0, 6), NULL);
    char_info->health = (uint32_t)strtoul(PQgetvalue(res, 0, 7), NULL, 10);
    char_info->max_health = (uint32_t)strtoul(PQgetvalue(res, 0, 8), NULL, 10);
    char_info->mana = (int32_t)strtol(PQgetvalue(res, 0, 9), NULL, 10);
    char_info->max_mana = (int32_t)strtol(PQgetvalue(res, 0, 10), NULL, 10);
    char_info->experience = (uint64_t)strtoull(PQgetvalue(res, 0, 11), NULL, 10);
    char_info->gold = (uint32_t)strtoul(PQgetvalue(res, 0, 12), NULL, 10);
    
    // Parse equipment
    char_info->helmet = (uint32_t)strtoul(PQgetvalue(res, 0, 13), NULL, 10);
    char_info->gloves = (uint32_t)strtoul(PQgetvalue(res, 0, 14), NULL, 10);
    char_info->chest_armor = (uint32_t)strtoul(PQgetvalue(res, 0, 15), NULL, 10);
    char_info->leggings = (uint32_t)strtoul(PQgetvalue(res, 0, 16), NULL, 10);
    char_info->boots = (uint32_t)strtoul(PQgetvalue(res, 0, 17), NULL, 10);
    char_info->main_hand = (uint32_t)strtoul(PQgetvalue(res, 0, 18), NULL, 10);
    char_info->second_hand = (uint32_t)strtoul(PQgetvalue(res, 0, 19), NULL, 10);
    char_info->blessing = (uint16_t)strtoul(PQgetvalue(res, 0, 20), NULL, 10);
    
    // Parse inventory array (PostgreSQL array format: {1,2,3,...})
    const char* inventory_str = PQgetvalue(res, 0, 21);
    
    // Initialize inventory to zeros
    memset(char_info->inventory, 0, sizeof(char_info->inventory));
    
    if (inventory_str && strlen(inventory_str) > 2) {
        // Parse PostgreSQL array format: {val1,val2,val3,...}
        const char* ptr = inventory_str + 1; // Skip opening '{'
        int slot = 0;
        
        while (*ptr && *ptr != '}' && slot < 150) {
            char* endptr;
            long value = strtol(ptr, &endptr, 10);
            
            if (ptr != endptr) {
                char_info->inventory[slot++] = (uint32_t)value;
                ptr = endptr;
                
                // Skip comma
                if (*ptr == ',') ptr++;
            } else {
                break;
            }
        }
    }
    
    PQclear(res);
    release_connection(conn);
    
    return 1;
}

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
    
    // Build inventory array string: {val1,val2,val3,...}
    char inventory_str[8192]; // 150 slots * ~10 chars each + overhead
    char* ptr = inventory_str;
    *ptr++ = '{';
    
    for (int i = 0; i < 150; i++) {
        int written = snprintf(ptr, sizeof(inventory_str) - (ptr - inventory_str),
                              "%u%s", char_info->inventory[i], (i < 149) ? "," : "");
        if (written < 0 || (size_t)written >= sizeof(inventory_str) - (ptr - inventory_str)) {
            fprintf(stderr, "Failed to build inventory string\n");
            PQexec(conn, "ROLLBACK");
            release_connection(conn);
            return 0;
        }
        ptr += written;
    }
    *ptr++ = '}';
    *ptr = '\0';
    
    // Prepare parameter values as strings
    char character_id_str[32], level_str[32], pos_x_str[32], pos_y_str[32];
    char health_str[32], max_health_str[32], mana_str[32], max_mana_str[32];
    char experience_str[32], gold_str[32];
    char helmet_str[32], gloves_str[32];
    char chest_str[32], leggings_str[32];
    char boots_str[32], main_hand_str[32];
    char second_hand_str[32], blessing_str[32];
    
    snprintf(character_id_str, sizeof(character_id_str), "%u", char_info->character_id);
    snprintf(level_str, sizeof(level_str), "%u", char_info->level);
    snprintf(pos_x_str, sizeof(pos_x_str), "%f", char_info->pos_x);
    snprintf(pos_y_str, sizeof(pos_y_str), "%f", char_info->pos_y);
    snprintf(health_str, sizeof(health_str), "%u", char_info->health);
    snprintf(max_health_str, sizeof(max_health_str), "%u", char_info->max_health);
    snprintf(mana_str, sizeof(mana_str), "%d", char_info->mana);
    snprintf(max_mana_str, sizeof(max_mana_str), "%d", char_info->max_mana);
    snprintf(experience_str, sizeof(experience_str), "%lu", char_info->experience);
    snprintf(gold_str, sizeof(gold_str), "%u", char_info->gold);
    
    snprintf(helmet_str, sizeof(helmet_str), "%u", char_info->helmet);
    snprintf(gloves_str, sizeof(gloves_str), "%u", char_info->gloves);
    snprintf(chest_str, sizeof(chest_str), "%u", char_info->chest_armor);
    snprintf(leggings_str, sizeof(leggings_str), "%u", char_info->leggings);
    snprintf(boots_str, sizeof(boots_str), "%u", char_info->boots);
    snprintf(main_hand_str, sizeof(main_hand_str), "%u", char_info->main_hand);
    snprintf(second_hand_str, sizeof(second_hand_str), "%u", char_info->second_hand);
    snprintf(blessing_str, sizeof(blessing_str), "%u", char_info->blessing);
    
    const char* param_values[19] = {
        level_str,          // $1
        pos_x_str,          // $2
        pos_y_str,          // $3
        health_str,         // $4
        max_health_str,     // $5
        mana_str,           // $6
        max_mana_str,       // $7
        experience_str,     // $8
        gold_str,           // $9
        helmet_str,         // $10
        gloves_str,         // $11
        chest_str,          // $12
        leggings_str,       // $13
        boots_str,          // $14
        main_hand_str,      // $15
        second_hand_str,    // $16
        blessing_str,       // $17
        inventory_str,      // $18
        character_id_str    // $19
    };
    
    res = PQexecParams(conn,
        "UPDATE characters SET "
        "level = $1, pos_x = $2, pos_y = $3, "
        "health = $4, max_health = $5, mana = $6, max_mana = $7, "
        "experience = $8, gold = $9, "
        "helmet = $10, gloves = $11, "
        "chest_armor = $12, leggings = $13, "
        "boots = $14, main_hand = $15, "
        "second_hand = $16, effect = $17, "
        "inventory = $18::integer[] "
        "WHERE character_id = $19",
        19,
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