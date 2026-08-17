#ifndef SESSION_H
#define SESSION_H

/** @file Expose Redis-backed sessions, authentication tokens, and player-state caching. */

#include "types.h"

#include <stdint.h>
#include <time.h>
#include <hiredis/hiredis.h>

/** Configure the default Redis endpoint and session-cache lifetimes. */
#define REDIS_HOST "127.0.0.1"
#define REDIS_PORT 6379
#define SESSION_EXPIRY_SECONDS 300
#define PLAYER_STATE_EXPIRY_SECONDS 300    /**< Hot-cache lifetime in seconds. */
#define SESSION_KEY_LENGTH 32
#define MAX_SESSIONS 10000

extern redisContext* g_redis;
extern pthread_mutex_t g_redis_lock;

/** Represent the fixed fields stored for one account session. */
typedef struct {
    uint32_t account_id;
    char session_key[SESSION_KEY_LENGTH];
    time_t created_at;
    time_t expires_at;
    char ip_address[16];
} Session;

int session_store(uint32_t player_id, const char* session_key);
void session_mark_active(uint32_t player_id);
void session_remove(uint32_t player_id);
void session_refresh(uint32_t player_id);
// return an allocated 32-character key that the caller frees
char* generate_session_key(void);


int session_init(void);
int session_create(uint32_t account_id, const char* ip_address, char* out_session_key);
int session_validate(uint32_t account_id, const char* session_key);
int session_invalidate(uint32_t account_id);
void session_cleanup_expired(void);
void session_close(void);

 
int session_cache_player_state(const ActivePlayer* player);
int session_load_cached_state(uint32_t player_id, ActivePlayer* player);
void session_clear_cached_state(uint32_t player_id);

int store_game_ticket_in_redis(const char* key, const char* value, int expiry_seconds);
int validate_game_ticket(const char* game_ticket, uint32_t* out_account_id, uint32_t* out_character_id, uint32_t* out_world_id);

// accept terminated 32-character tokens and consume them once
int     auth_token_store(const char* token, uint32_t player_id);
uint32_t auth_token_consume(const char* token);

#endif