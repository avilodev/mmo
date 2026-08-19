#ifndef WORLD_DATABASE_MANAGER_H
#define WORLD_DATABASE_MANAGER_H

#include "types.h"

#include <stdint.h>
#include <libpq-fe.h>
#include <time.h>
#include <errno.h>

/** Set the fixed PostgreSQL connection count allocated per world. */
#define CONN_PER_WORLD 4
/** Bound the world database pools retained by the realm. */
#define MAX_WORLDS 10

/** Track one lock-protected connection slot in a world pool. */
typedef struct {
    PGconn* conn;
    int in_use;
    pthread_mutex_t lock;
} WorldConnection;

/** Coordinate a world's bounded PostgreSQL connection pool. */
typedef struct {
    uint32_t world_id;
    char db_name[64];
    WorldConnection connections[CONN_PER_WORLD];
    int initialized;
    pthread_mutex_t pool_lock;
    pthread_cond_t conn_available;
} WorldDatabasePool;


int world_character_get_list(uint32_t account_id, uint32_t world_id,
                             CharacterInfo* characters, int max_count);
int world_character_count(uint32_t account_id, uint32_t world_id);

int world_character_create(uint32_t account_id, uint32_t world_id,
                           const char* name, int class_id, int race_id,
                           uint32_t* out_character_id);
int world_character_belongs_to_account(uint32_t character_id, uint32_t account_id, uint32_t world_id);
int world_character_delete(uint32_t account_id, uint32_t character_id, 
                           uint32_t world_id);
void world_databases_cleanup(void);

#endif  
