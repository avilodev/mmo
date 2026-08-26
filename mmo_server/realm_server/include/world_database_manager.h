#ifndef WORLD_DATABASE_MANAGER_H
#define WORLD_DATABASE_MANAGER_H

#include "types.h"

#include <stdatomic.h>
#include <stdint.h>
#include <libpq-fe.h>
#include <time.h>
#include <errno.h>

/** Default PostgreSQL connections opened per world.
 *
 * A default, not a ceiling: `$MMO_WORLD_POOL_SIZE` overrides it at startup and
 * each pool allocates that many slots. It is a default because the right
 * number depends on how many realm workers can be in a character query at
 * once, which is a deployment fact rather than a compile-time one.
 */
#define CONN_PER_WORLD_DEFAULT 4

/** Report the per-world connection count this process will open.
 *
 * `$MMO_WORLD_POOL_SIZE` overrides the default above, so the compiled constant
 * is not the answer. Safe to call before the pools exist: the realm's blocking
 * worker pool is sized from this, and it has to be sized before it is started.
 */
int world_database_manager_conn_per_world(void);

/** Track one lock-protected connection slot in a world pool. */
typedef struct {
    PGconn* conn;
    int in_use;
    pthread_mutex_t lock;
} WorldConnection;

/** Coordinate a world's bounded PostgreSQL connection pool.
 *
 * Each pool carries its own init_lock. Opening a pool means CONN_PER_WORLD
 * PostgreSQL handshakes, and those used to run under one lock shared by every
 * world: the first character-screen request for any world stalled the first
 * request for all the others behind it, on a realm server that is
 * thread-per-connection and therefore had each of those requests on its own
 * thread with nothing else to do.
 */
typedef struct {
    uint32_t world_id;
    char db_name[64];

    /** `conn_count` slots, allocated when the process sizes its pools.
     *
     * Heap rather than a fixed array so the count is a runtime decision and so
     * the pool table itself can be sized from the world roster instead of from
     * a compiled maximum world count.
     */
    WorldConnection* connections;
    int conn_count;

    /** Published with release ordering once every connection is open. */
    _Atomic int initialized;

    /** Serializes opening this one pool; never held across another world's. */
    pthread_mutex_t init_lock;

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

/** Number of world pools the realm allocated, one per configured world. */
size_t world_database_pool_count(void);

#endif  
