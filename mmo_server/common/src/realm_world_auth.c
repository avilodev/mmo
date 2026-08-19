/**
 * @file
 * Store and validate realm-to-world authentication data in Redis.
 */
#include "realm_world_auth.h"
#include "session.h"

#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <hiredis/hiredis.h>

/**
 * Retrieve a server-to-server authentication key by server name.
 *
 * The caller must free the returned string.
 *
 * @return      The allocated key, or NULL when unavailable.
 */
char* get_server_auth_key_from_redis(const char* server_name) {
    if (!g_redis || !server_name) {
        return NULL;
    }
    
    char redis_key[128];
    snprintf(redis_key, sizeof(redis_key), "server_auth_key:%s", server_name);
    
    pthread_mutex_lock(&g_redis_lock);
    redisReply* reply = redisCommand(g_redis, "GET %s", redis_key);
    pthread_mutex_unlock(&g_redis_lock);
    
    if (!reply || reply->type != REDIS_REPLY_STRING) {
        if (reply) freeReplyObject(reply);
        return NULL;
    }
    
    char* auth_key = strdup(reply->str);
    freeReplyObject(reply);
    
    return auth_key;
}

/**
 * Store a server authentication key with an optional Redis expiry.
 *
 * @param ttl_seconds  Lifetime in seconds, or a nonpositive value for no expiry.
 * @return             Nonzero when Redis accepts the key, otherwise zero.
 */
int set_server_auth_key_in_redis(const char* server_name, const char* auth_key, int ttl_seconds) {
    if (!g_redis || !server_name || !auth_key) {
        return 0;
    }
    
    char redis_key[128];
    snprintf(redis_key, sizeof(redis_key), "server_auth_key:%s", server_name);
    
    redisReply* reply;
    
    pthread_mutex_lock(&g_redis_lock);
    if (ttl_seconds > 0) {
        // Set with expiration
        reply = redisCommand(g_redis, "SETEX %s %d %s", redis_key, ttl_seconds, auth_key);
    } else {
        // Set without expiration
        reply = redisCommand(g_redis, "SET %s %s", redis_key, auth_key);
    }
    pthread_mutex_unlock(&g_redis_lock);
    
    int success = 0;
    if (reply && (reply->type == REDIS_REPLY_STATUS || reply->type == REDIS_REPLY_STRING)) {
        success = 1;
        printf("Set server auth key: %s (TTL: %d seconds)\n", redis_key, ttl_seconds);
    } else {
        fprintf(stderr, "Failed to set server auth key: %s\n", redis_key);
    }
    
    if (reply) freeReplyObject(reply);
    return success;
}

/**
 * Compare a provided server key with the value stored for a world.
 *
 * @return      Nonzero for a matching stored key, otherwise zero.
 */
int validate_server_auth_key(const char* provided_key, const char* world_name) {
    char* expected_key = get_server_auth_key_from_redis(world_name);
    
    if (!expected_key) {
        return 0;
    }

    int valid = (strcmp(provided_key, expected_key) == 0);
    free(expected_key);
    
    return valid;
}

/**
 * Read the account identifier from a Redis game-ticket hash.
 *
 * @return      The stored account identifier, or zero when unavailable or invalid.
 */
uint32_t get_account_from_ticket(const char* game_ticket) {
    if (!game_ticket || !g_redis) {
        fprintf(stderr, "get_account_from_ticket: invalid parameters\n");
        return 0;
    }
    
    // Build Redis key: "game_ticket:<ticket_value>"
    char key[256];
    snprintf(key, sizeof(key), "game_ticket:%s", game_ticket);
    
    // Get the account_id field from the hash
    pthread_mutex_lock(&g_redis_lock);
    redisReply* reply = redisCommand(g_redis, "HGET %s account_id", key);
    pthread_mutex_unlock(&g_redis_lock);
    if (!reply) {
        fprintf(stderr, "Redis error getting account_id from ticket\n");
        return 0;
    }
    
    if (reply->type != REDIS_REPLY_STRING) {
        freeReplyObject(reply);
        return 0;
    }
    
    uint32_t account_id = (uint32_t)strtoul(reply->str, NULL, 10);
    freeReplyObject(reply);
    
    if (account_id == 0) {
        fprintf(stderr, "Invalid account_id in ticket: %s\n", game_ticket);
    }
    
    return account_id;
}
