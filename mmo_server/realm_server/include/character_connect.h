#ifndef CHARACTER_MANAGER_H
#define CHARACTER_MANAGER_H

#include "types.h"
#include <stdint.h>

// Database initialization
int character_database_init(const char* pg_conn_str);
void character_database_close(void);

// Character management functions
void handle_character_list_request(int client_fd, uint32_t account_id, uint32_t world_id);
void handle_character_create_request(int client_fd, uint32_t account_id, 
                                      uint32_t world_id, const char* name, 
                                      int class_id, int race_id);
void handle_character_delete_request(int client_fd, uint32_t account_id, 
                                      uint32_t character_id, uint32_t world_id);

#endif
