#ifndef WORLD_DATABASE_MANAGER_H
#define WORLD_DATABASE_MANAGER_H

#include "character_record.h"
#include "types.h"

#include <stdint.h>
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

int world_character_get_list(uint32_t account_id, uint32_t world_id,
                             character_record* characters, int max_count);
int world_character_count(uint32_t account_id, uint32_t world_id);

int world_character_create(uint32_t account_id, uint32_t world_id,
                           const char* name, int class_id, int race_id,
                           uint32_t appearance, uint32_t* out_character_id);
int world_character_belongs_to_account(uint32_t character_id, uint32_t account_id, uint32_t world_id);
int world_character_delete(uint32_t account_id, uint32_t character_id, 
                           uint32_t world_id);
void world_databases_cleanup(void);

#endif  
