#ifndef DATABASE_OPERATIONS_H
#define DATABASE_OPERATIONS_H

#include "types.h"

#include <stdint.h>
#include <libpq-fe.h>
#include <time.h>
#include <errno.h>
 
// Initialize the character database connection pool
int character_database_init(const char* connection_string);

// Close all database connections
void character_database_close(void);

// Get list of characters for a specific account and world
int character_get_list_for_world(uint32_t account_id, uint32_t world_id, 
                                  CharacterInfo* characters, int max_count);

// Count characters for an account in a specific world
int character_count_in_world(uint32_t account_id, uint32_t world_id);

// Create a new character in a world
int character_create_in_world(uint32_t account_id, uint32_t world_id, 
                              const char* name, int class_id, int race_id, 
                              uint32_t* out_character_id);

// Delete a character from a world
int character_delete_from_world(uint32_t account_id, uint32_t character_id, 
                                uint32_t world_id);

// Check if a character belongs to an account
int character_belongs_to_account(uint32_t character_id, uint32_t account_id);

// Get full character data including equipment
int character_get_full_data(uint32_t character_id, CharacterInfo* char_info);

// Update full character data including equipment
int character_update_full_data(const CharacterInfo* char_info);

// NEW: Get the account_id that owns a character
uint32_t character_get_owner(uint32_t character_id);

#endif // DATABASE_OPERATIONS_H