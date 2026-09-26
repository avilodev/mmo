#ifndef DATABASE_OPERATIONS_H
#define DATABASE_OPERATIONS_H

/** @file Expose pooled PostgreSQL persistence for characters and owned item instances. */

#include "types.h"
#include "item_instance.h"

#include <stdint.h>
#include <libpq-fe.h>
#include <time.h>
#include <errno.h>

/** Bound the PostgreSQL connections every character query shares.
 *
 * Declared here rather than hidden in the implementation because it is not an
 * internal detail: it is the real concurrency limit of every service that
 * touches character data, and acquire_connection() does not queue past it
 * politely -- it waits five seconds and then fails the request. Anything that
 * sizes a worker pool in front of this must size it from this, or it converts
 * a harmless wait into a timed-out query under exactly the load that caused it.
 */
#define DB_CONN_POOL_SIZE 4

/** Report the pool size this process will open.
 *
 * `$MMO_DB_POOL_SIZE` overrides the default above at startup, so the compiled
 * constant is not the answer. Safe to call before character_database_init():
 * anything sizing a worker pool in front of the database needs the number
 * first, and it reads the same environment init does.
 */
int character_database_pool_configured_size(void);
 
int character_database_init(const char* connection_string);

/** Report connection-pool saturation, for the metrics endpoint.
 *
 * A pool that is permanently full is the shape every database-bound stall
 * takes, and until this existed there was no way to see it from outside the
 * process: queries simply got slower and then began timing out after five
 * seconds with nothing to say why.
 *
 * @param out_in_use   Connections currently checked out. May be NULL.
 * @param out_size     Pool size. May be NULL.
 * @param out_waiters  Threads parked waiting for one. May be NULL.
 */
void character_database_pool_stats(int* out_in_use, int* out_size,
                                   int* out_waiters);

void character_database_close(void);

int character_get_list_for_world(uint32_t account_id, uint32_t world_id, 
                                  CharacterInfo* characters, int max_count);

int character_count_in_world(uint32_t account_id, uint32_t world_id);

int character_create_in_world(uint32_t account_id, uint32_t world_id, 
                              const char* name, int class_id, int race_id, 
                              uint32_t* out_character_id);

int character_delete_from_world(uint32_t account_id, uint32_t character_id, 
                                uint32_t world_id);

int character_belongs_to_account(uint32_t character_id, uint32_t account_id);

int character_get_full_data(uint32_t character_id, CharacterInfo* char_info);

int character_update_full_data(const CharacterInfo* char_info);

/** Commit a character's scalars, currency balances and items in one transaction.
 *
 * character_update_full_data() and character_items_save() each open their own
 * transaction, so calling them in sequence -- which is what saving a character
 * means -- left a window where a failure between the two persisted currency
 * without the items it bought, or the reverse, with nothing afterwards able to
 * tell. Use this for a whole-character save; the two narrower functions remain
 * for the cases that genuinely write only one half.
 *
 * @return 1 after commit, or 0 on invalid input or database failure.
 */
int character_save_all(const CharacterInfo* char_info,
                       const ItemInstance* inventory, int inventory_slots,
                       const ItemInstance* equipment, int equip_slots);

// return zero when the world contains no persisted item instances
//
// Zero is ambiguous: it is returned both for an empty table and for a failed
// query. Startup seeding must not treat a failed query as "no items", so use
// character_items_max_instance_id_checked() there and keep this one for tests
// and diagnostics.
uint64_t character_items_max_instance_id(void);

// report query success separately from the value, so a failed lookup cannot be
// mistaken for an empty table; returns 1 on success, 0 on query failure
int character_items_max_instance_id_checked(uint64_t* out_highest);

// overwrite both arrays and zero slots absent from the database
int character_items_load(uint32_t character_id,
                         ItemInstance* inventory, int inventory_slots,
                         ItemInstance* equipment, int equip_slots);

// replace all item rows atomically while retaining surviving identifiers
int character_items_save(uint32_t character_id,
                         const ItemInstance* inventory, int inventory_slots,
                         const ItemInstance* equipment, int equip_slots);

uint32_t character_get_owner(uint32_t character_id);

#endif // DATABASE_OPERATIONS_H