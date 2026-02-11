#ifndef REALM_WORLD_AUTH_H
#define REALM_WORLD_AUTH_H

#include <stdint.h>
#include <hiredis/hiredis.h>

extern redisContext* g_redis;

// Server-to-server authentication
char* get_server_auth_key_from_redis(const char* world_name);
int set_server_auth_key_in_redis(const char* server_name, const char* auth_key, int ttl_seconds);
int validate_server_auth_key(const char* provided_key, const char* world_name);

int validate_game_ticket(const char* game_ticket, uint32_t* out_account_id, uint32_t* out_character_id, uint32_t* out_world_id);
uint32_t get_account_from_ticket(const char* game_ticket);

#endif