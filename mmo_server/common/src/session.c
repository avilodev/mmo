#define _POSIX_C_SOURCE 200809L

#include "session.h"
#include "types.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <hiredis/hiredis.h>

redisContext* g_redis = NULL;
pthread_mutex_t g_redis_lock = PTHREAD_MUTEX_INITIALIZER;

int session_init(void) {
    const char* redis_host = getenv("REDIS_HOST");
    if (!redis_host) {
        redis_host = "127.0.0.1";
    }
    
    int redis_port = 6379;
    const char* redis_port_str = getenv("REDIS_PORT");
    if (redis_port_str) {
        redis_port = atoi(redis_port_str);
    }
    
    g_redis = redisConnect(redis_host, redis_port);
    
    if (g_redis == NULL || g_redis->err) {
        if (g_redis) {
            fprintf(stderr, "Redis connection error: %s\n", g_redis->errstr);
            redisFree(g_redis);
            g_redis = NULL;
        } else {
            fprintf(stderr, "Redis connection error: can't allocate context\n");
        }
        return 0;
    }
    
    printf("Session system initialized (Redis: %s:%d)\n", redis_host, redis_port);
    return 1;
}

int session_create(uint32_t account_id, const char* ip_address, char* out_session_key) {
    if (!g_redis || !out_session_key) {
        return 0;
    }
    
    char* session_key = generate_session_key();
    if (!session_key) {
        return 0;
    }
    
    char redis_key[64];
    snprintf(redis_key, sizeof(redis_key), "session:%u", account_id);
    
    time_t now = time(NULL);
    time_t expires_at = now + SESSION_EXPIRY_SECONDS;
    
    // Create a temporary buffer with EXACTLY 32 bytes (no null terminator)
    char key_buffer[32];
    memcpy(key_buffer, session_key, 32);
    
    pthread_mutex_lock(&g_redis_lock);
    
    // Use %b for binary-safe session key storage (exactly 32 bytes)
    redisReply* reply = redisCommand(g_redis, 
        "HMSET %s session_key %b created_at %ld expires_at %ld ip_address %s",
        redis_key, key_buffer, (size_t)32, now, expires_at,
        ip_address ? ip_address : "unknown"
    );
    
    if (!reply || reply->type == REDIS_REPLY_ERROR) {
        if (reply) {
            printf("Redis error in session_create: %s\n", reply->str);
            freeReplyObject(reply);
        }
        pthread_mutex_unlock(&g_redis_lock);
        free(session_key);
        return 0;
    }
    freeReplyObject(reply);
    
    reply = redisCommand(g_redis, "EXPIRE %s %d", redis_key, SESSION_EXPIRY_SECONDS);
    if (reply) freeReplyObject(reply);
    
    pthread_mutex_unlock(&g_redis_lock);
    
    memcpy(out_session_key, key_buffer, 32);
    
    //printf("Created session for account %u (key: %s, 32 bytes, expires in %d seconds)\n", 
    //       account_id, redis_key, SESSION_EXPIRY_SECONDS);
    //printf("DEBUG: Session key bytes being stored: ");
    //for (int i = 0; i < 32; i++) {
    //    printf("%c", key_buffer[i]);
    //}
    //printf("\n");
    //printf("DEBUG: Session key bytes being returned: ");
    //for (int i = 0; i < 32; i++) {
    //    printf("%c", out_session_key[i]);
    //}
    //printf("\n");
    
    free(session_key);
    return 1;
}

int session_validate(uint32_t account_id, const char* session_key) {
    if (!g_redis || !session_key) {
        printf("Session validate: Invalid parameters (redis=%p, key=%p)\n", 
               (void*)g_redis, (void*)session_key);
        return 0;
    }
    
    char redis_key[64];
    snprintf(redis_key, sizeof(redis_key), "session:%u", account_id);
    
    pthread_mutex_lock(&g_redis_lock);
    
    // First check if key exists
    redisReply* exists_reply = redisCommand(g_redis, "EXISTS %s", redis_key);
    if (!exists_reply || exists_reply->type != REDIS_REPLY_INTEGER || exists_reply->integer == 0) {
        printf("Session validate: Key '%s' does not exist in Redis\n", redis_key);
        if (exists_reply) freeReplyObject(exists_reply);
        pthread_mutex_unlock(&g_redis_lock);
        return 0;
    }
    freeReplyObject(exists_reply);
    
    // Get the session_key field from hash
    redisReply* reply = redisCommand(g_redis, "HGET %s session_key", redis_key);
    
    if (!reply) {
        printf("Session validate: Redis command failed for HGET %s session_key\n", redis_key);
        pthread_mutex_unlock(&g_redis_lock);
        return 0;
    }
    
    if (reply->type != REDIS_REPLY_STRING) {
        printf("Session validate: Key '%s' is not a hash (type=%d). Trying simple GET...\n", 
               redis_key, reply->type);
        freeReplyObject(reply);
        
        // Try simple GET for backward compatibility with session_store
        reply = redisCommand(g_redis, "GET %s", redis_key);
        if (!reply || reply->type != REDIS_REPLY_STRING) {
            printf("Session validate: GET also failed\n");
            if (reply) freeReplyObject(reply);
            pthread_mutex_unlock(&g_redis_lock);
            return 0;
        }
        
        // Compare with simple string value
        size_t stored_len = reply->len;
        int valid = (stored_len == SESSION_KEY_LENGTH && 
                     memcmp(reply->str, session_key, SESSION_KEY_LENGTH) == 0);
        
        freeReplyObject(reply);
        pthread_mutex_unlock(&g_redis_lock);
        
        if (valid) {
            printf("Session validated successfully for account %u (simple format)\n", account_id);
        }
        
        return valid;
    }
    
    // Hash format validation
    size_t stored_len = reply->len;

    // Compare exactly 32 bytes (SESSION_KEY_LENGTH)
    int valid = (stored_len >= SESSION_KEY_LENGTH && 
                 memcmp(reply->str, session_key, SESSION_KEY_LENGTH) == 0);
    
    freeReplyObject(reply);
    
    if (!valid) {
        printf("Session validate: Key mismatch!\n");
        pthread_mutex_unlock(&g_redis_lock);
        return 0;
    }
    
    // Check expiry
    reply = redisCommand(g_redis, "HGET %s expires_at", redis_key);
    if (!reply || reply->type != REDIS_REPLY_STRING) {
        printf("Session validate: No expires_at field, assuming valid\n");
        if (reply) freeReplyObject(reply);
        pthread_mutex_unlock(&g_redis_lock);
        printf("Session validated successfully for account %u (hash format, no expiry check)\n", account_id);
        return 1;
    }
    
    time_t expires_at = atol(reply->str);
    freeReplyObject(reply);
    
    time_t now = time(NULL);
    if (now > expires_at) {
        printf("Session validate: Session expired (now=%ld, expires=%ld)\n", now, expires_at);
        redisReply* del_reply = redisCommand(g_redis, "DEL %s", redis_key);
        if (del_reply) freeReplyObject(del_reply);
        pthread_mutex_unlock(&g_redis_lock);
        return 0;
    }

    // Refresh expiry
    reply = redisCommand(g_redis, "EXPIRE %s %d", redis_key, SESSION_EXPIRY_SECONDS);
    if (reply) freeReplyObject(reply);
    
    pthread_mutex_unlock(&g_redis_lock);
    
    printf("Session validated successfully for account %u (hash format with expiry)\n", account_id);
    return 1;
}

int session_invalidate(uint32_t account_id) {
    if (!g_redis) {
        return 0;
    }
    
    char redis_key[64];
    snprintf(redis_key, sizeof(redis_key), "session:%u", account_id);
    
    pthread_mutex_lock(&g_redis_lock);
    redisReply* reply = redisCommand(g_redis, "DEL %s", redis_key);
    
    int success = 0;
    if (reply && reply->type == REDIS_REPLY_INTEGER) {
        success = (reply->integer > 0);
    }
    
    if (reply) freeReplyObject(reply);
    pthread_mutex_unlock(&g_redis_lock);
    return success;
}

void session_cleanup_expired(void) {
    if (!g_redis) {
        return;
    }
    
    pthread_mutex_lock(&g_redis_lock);
    
    redisReply* reply = redisCommand(g_redis, "KEYS session:*");
    
    if (!reply || reply->type != REDIS_REPLY_ARRAY) {
        if (reply) freeReplyObject(reply);
        pthread_mutex_unlock(&g_redis_lock);
        return;
    }
    
    int expired_count = 0;
    time_t now = time(NULL);
    
    for (size_t i = 0; i < reply->elements; i++) {
        const char* key = reply->element[i]->str;
        
        redisReply* exp_reply = redisCommand(g_redis, "HGET %s expires_at", key);
        if (exp_reply && exp_reply->type == REDIS_REPLY_STRING) {
            time_t expires_at = atol(exp_reply->str);
            
            if (now > expires_at) {
                redisReply* del_r = redisCommand(g_redis, "DEL %s", key);
                if (del_r) freeReplyObject(del_r);
                expired_count++;
            }
        }
        
        if (exp_reply) freeReplyObject(exp_reply);
    }
    
    freeReplyObject(reply);
    pthread_mutex_unlock(&g_redis_lock);
    
    if (expired_count > 0) {
        printf("Cleaned up %d expired sessions\n", expired_count);
    }
}

void session_close(void) {
    if (g_redis) {
        redisFree(g_redis);
        g_redis = NULL;
        printf("Session system closed\n");
    }
}

// FIXED: Use HMSET format to match session_validate
int session_store(uint32_t player_id, const char* session_key) {
    if (!g_redis || !session_key || player_id == 0) {
        return 0;
    }
    
    char key[64];
    snprintf(key, sizeof(key), "session:%u", player_id);
    
    time_t now = time(NULL);
    time_t expires_at = now + SESSION_EXPIRY_SECONDS;
    
    // Create a temporary buffer with EXACTLY 32 bytes (no null terminator)
    char key_buffer[32];
    memcpy(key_buffer, session_key, 32);
    
    pthread_mutex_lock(&g_redis_lock);
    
    // Use HMSET format with binary-safe key storage (exactly 32 bytes)
    // Format the session_key as a binary-safe string by using %b with explicit length
    redisReply *reply = redisCommand(g_redis, 
        "HMSET %s session_key %b created_at %ld expires_at %ld",
        key, key_buffer, (size_t)32, now, expires_at);
    
    if (!reply || reply->type == REDIS_REPLY_ERROR) {
        if (reply) {
            printf("Redis error in session_store: %s\n", reply->str);
            freeReplyObject(reply);
        }
        pthread_mutex_unlock(&g_redis_lock);
        printf("Failed to store session for player %u\n", player_id);
        return 0;
    }
    freeReplyObject(reply);
    
    // Set expiry on the hash
    reply = redisCommand(g_redis, "EXPIRE %s %d", key, SESSION_EXPIRY_SECONDS);
    if (reply) freeReplyObject(reply);
    
    pthread_mutex_unlock(&g_redis_lock);
    
    printf("Stored session for player %u using HMSET format (32 bytes)\n", player_id);
    return 1;
}

void session_mark_active(uint32_t player_id) {
    if (!g_redis || player_id == 0) {
        return;
    }
    
    char key[64];
    snprintf(key, sizeof(key), "session:%u:active", player_id);
    
    pthread_mutex_lock(&g_redis_lock);
    redisReply *reply = redisCommand(g_redis, "SETEX %s %d 1", 
                                     key, SESSION_EXPIRY_SECONDS);
    
    if (reply) freeReplyObject(reply);
    pthread_mutex_unlock(&g_redis_lock);
}

void session_remove(uint32_t player_id) {
    if (!g_redis || player_id == 0) {
        return;
    }
    
    char session_key[64];
    char active_key[64];
    snprintf(session_key, sizeof(session_key), "session:%u", player_id);
    snprintf(active_key, sizeof(active_key), "session:%u:active", player_id);
    
    pthread_mutex_lock(&g_redis_lock);
    redisReply *reply = redisCommand(g_redis, "DEL %s %s", session_key, active_key);
    
    if (reply) freeReplyObject(reply);
    pthread_mutex_unlock(&g_redis_lock);
}

void session_refresh(uint32_t player_id) {
    if (!g_redis || player_id == 0) {
        return;
    }
    
    char key[64];
    snprintf(key, sizeof(key), "session:%u", player_id);
    
    pthread_mutex_lock(&g_redis_lock);
    redisReply *reply = redisCommand(g_redis, "EXPIRE %s %d", key, SESSION_EXPIRY_SECONDS);
    if (reply) freeReplyObject(reply);
    pthread_mutex_unlock(&g_redis_lock);
}

char* generate_session_key(void) {
    const char charset[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    const size_t charset_size = 62;
    char* key = malloc(33); 
    
    if (!key) return NULL;
    
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) {
        free(key);
        return NULL;
    }
    
    unsigned char random_bytes[32];
    if (read(fd, random_bytes, 32) != 32) {
        close(fd);
        free(key);
        return NULL;
    }
    close(fd);
     
    for (int i = 0; i < 32; i++) {
        key[i] = charset[random_bytes[i] % charset_size];
    }
    key[32] = '\0';
    
    return key;
}

int store_game_ticket_in_redis(const char* key, const char* value, int expiry_seconds) {
    if (!g_redis) return 0;
    
    pthread_mutex_lock(&g_redis_lock);
    redisReply* reply = redisCommand(g_redis, "SETEX %s %d %s", 
                                     key, expiry_seconds, value);
    if (!reply) {
        pthread_mutex_unlock(&g_redis_lock);
        return 0;
    }
    
    int success = (reply->type == REDIS_REPLY_STATUS && 
                   strcmp(reply->str, "OK") == 0);
    freeReplyObject(reply);
    pthread_mutex_unlock(&g_redis_lock);
    return success;
}

// ---------------------------------------------------------------------------
// Stage 1 → Stage 2 auth token management
// ---------------------------------------------------------------------------

int auth_token_store(const char* token, uint32_t player_id) {
    if (!g_redis || !token) return 0;

    char key[80];
    snprintf(key, sizeof(key), "auth_token:%.32s", token);

    char value[32];
    snprintf(value, sizeof(value), "%u", player_id);

    pthread_mutex_lock(&g_redis_lock);
    redisReply* reply = redisCommand(g_redis, "SETEX %s 60 %s", key, value);
    int ok = (reply && reply->type == REDIS_REPLY_STATUS &&
              strcmp(reply->str, "OK") == 0);
    if (reply) freeReplyObject(reply);
    pthread_mutex_unlock(&g_redis_lock);

    return ok;
}

uint32_t auth_token_consume(const char* token) {
    if (!g_redis || !token) return 0;

    char key[80];
    snprintf(key, sizeof(key), "auth_token:%.32s", token);

    pthread_mutex_lock(&g_redis_lock);

    // Fetch and delete atomically. A mutex alone is insufficient when more
    // than one login-server process shares Redis.
    static const char consume_script[] =
        "local v=redis.call('GET',KEYS[1]); "
        "if v then redis.call('DEL',KEYS[1]) end; return v";
    redisReply* reply = redisCommand(g_redis, "EVAL %b 1 %s",
                                     consume_script, strlen(consume_script), key);
    if (!reply || reply->type != REDIS_REPLY_STRING) {
        if (reply) freeReplyObject(reply);
        pthread_mutex_unlock(&g_redis_lock);
        return 0;
    }

    uint32_t player_id = (uint32_t)strtoul(reply->str, NULL, 10);
    freeReplyObject(reply);

    pthread_mutex_unlock(&g_redis_lock);

    return player_id;
}

// ---------------------------------------------------------------------------

int validate_game_ticket(const char* ticket, uint32_t* out_account_id, uint32_t* out_character_id, uint32_t* out_world_id) {
    if (!g_redis || !ticket) return 0;
    
    char ticket_key[128];
    snprintf(ticket_key, sizeof(ticket_key), "ticket:%s", ticket);
    
    pthread_mutex_lock(&g_redis_lock);
    redisReply* reply = redisCommand(g_redis, "GET %s", ticket_key);
    
    if (!reply || reply->type != REDIS_REPLY_STRING) {
        if (reply) freeReplyObject(reply);
        pthread_mutex_unlock(&g_redis_lock);
        return 0;
    }
    
    // Ticket is stored by realm as: "character_id:world_id:account_id"
    uint32_t char_id, world_id, account_id;
    if (sscanf(reply->str, "%u:%u:%u", &char_id, &world_id, &account_id) != 3) {
        fprintf(stderr, "Failed to parse game ticket: '%s'\n", reply->str);
        freeReplyObject(reply);
        pthread_mutex_unlock(&g_redis_lock);
        return 0;
    }
    
    freeReplyObject(reply);
    
    // Delete the ticket (single-use)
    reply = redisCommand(g_redis, "DEL %s", ticket_key);
    if (reply) freeReplyObject(reply);
    
    pthread_mutex_unlock(&g_redis_lock);
    
    if (out_account_id)   *out_account_id = account_id;
    if (out_character_id)  *out_character_id = char_id;
    if (out_world_id)      *out_world_id = world_id;
    
    printf("Game ticket validated: account=%u, char=%u, world=%u\n", account_id, char_id, world_id);
    return 1;
}
