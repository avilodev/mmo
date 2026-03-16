#ifndef SESSION_H
#define SESSION_H

#include "types.h"

#include <stdint.h>
#include <time.h>
#include <hiredis/hiredis.h>

#define REDIS_HOST "127.0.0.1"
#define REDIS_PORT 6379
#define SESSION_EXPIRY_SECONDS 300
#define PLAYER_STATE_EXPIRY_SECONDS 300    // 5 minutes (hot cache)
#define SESSION_KEY_LENGTH 32
#define MAX_SESSIONS 10000

extern redisContext* g_redis;
extern pthread_mutex_t g_redis_lock;

// Session stored in Redis with unique key per account
typedef struct {
    uint32_t account_id;
    char session_key[SESSION_KEY_LENGTH];
    time_t created_at;
    time_t expires_at;
    char ip_address[16];
} Session;

// Session management
int session_store(uint32_t player_id, const char* session_key);
void session_mark_active(uint32_t player_id);
void session_remove(uint32_t player_id);
void session_refresh(uint32_t player_id);
char* generate_session_key(void);


// Initialize session system (connects to Redis)
int session_init(void);
int session_create(uint32_t account_id, const char* ip_address, char* out_session_key);
int session_validate(uint32_t account_id, const char* session_key);
int session_invalidate(uint32_t account_id);
void session_cleanup_expired(void);
void session_close(void);

 
// Player state caching in Redis (hot data)
int session_cache_player_state(const ActivePlayer* player);
int session_load_cached_state(uint32_t player_id, ActivePlayer* player);
void session_clear_cached_state(uint32_t player_id);

int store_game_ticket_in_redis(const char* key, const char* value, int expiry_seconds);
int validate_game_ticket(const char* game_ticket, uint32_t* out_account_id, uint32_t* out_character_id, uint32_t* out_world_id);

// Stage 1 → Stage 2 auth token (one-time, 60s TTL)
// token must be a null-terminated 32-char alphanumeric string (from generate_session_key)
int     auth_token_store(const char* token, uint32_t player_id);
uint32_t auth_token_consume(const char* token);

#endif