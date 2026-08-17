#ifndef DATABASE_OPERATIONS_H
#define DATABASE_OPERATIONS_H

/** @file Expose pooled PostgreSQL persistence for characters and owned item instances. */

#include "types.h"
#include "item_instance.h"

#include <stdint.h>
#include <libpq-fe.h>
#include <time.h>
#include <errno.h>
 
int character_database_init(const char* connection_string);

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

// return zero when the world contains no persisted item instances
uint64_t character_items_max_instance_id(void);

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