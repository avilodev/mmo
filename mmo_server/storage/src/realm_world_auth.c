/**
 * @file
 * Store and validate realm-to-world authentication data in Redis.
 *
 * Every Redis call here goes through redis_command_locked(), which owns the
 * sticky-context heal and reconnect. This file used to call redisCommand()
 * directly, so it inherited none of that: one Redis blip left the shared
 * context broken and every realm-to-world handshake failed from then on, which
 * takes the world list offline without a single error naming Redis.
 */
#include "realm_world_auth.h"
#include "session.h"
#include "log.h"

#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "redis_internal.h"

/** Redis key holding the key a server is authenticating with right now. */
#define AUTH_KEY_FMT          "server_auth_key:%s"

/** Redis key holding the key that was current before the last rotation.
 *
 * Rotation is not instantaneous across a fleet: the realm caches a key for a
 * cycle, a world reads its own, and the two can straddle the moment the value
 * changed. Keeping the previous key valid for a short overlap turns that from
 * an outage into a non-event. Without it, a world answers "Server key
 * unavailable" and drops off the world list with nothing said about why.
 */
#define AUTH_KEY_PREVIOUS_FMT "server_auth_key:%s:previous"

/**
 * Compare two NUL-terminated keys without leaking the matching prefix length.
 *
 * strcmp returns at the first differing byte, so how long it takes to answer
 * describes how much of a guess was right -- a usable oracle against a secret
 * that can be resubmitted. This lived as a static in world_server/main.c;
 * it is here so both ends of the handshake share one implementation.
 *
 * @return Nonzero when the keys are equal.
 */
int auth_key_equal(const char* a, const char* b) {
    if (!a || !b) return 0;

    size_t len_a = strlen(a);
    size_t len_b = strlen(b);

    /* Length is not secret -- it is the key's fixed encoding -- but the bytes
     * are, so the loop always runs over the full first string. */
    unsigned char diff = (unsigned char)((len_a ^ len_b) != 0);
    for (size_t i = 0; i < len_a; i++) {
        unsigned char rhs = (i < len_b) ? (unsigned char)b[i] : 0;
        diff |= (unsigned char)((unsigned char)a[i] ^ rhs);
    }
    return diff == 0;
}

/** Fetch one Redis string value, healing the context if needed.
 *
 * @return An allocated copy the caller frees, or NULL when absent.
 */
static char* fetch_key(const char* redis_key) {
    redis_pool_acquire();
    redisReply* reply = redis_command_locked("GET %s", redis_key);
    redis_pool_release();

    if (!reply || reply->type != REDIS_REPLY_STRING) {
        if (reply) freeReplyObject(reply);
        return NULL;
    }

    char* value = strdup(reply->str);
    freeReplyObject(reply);
    return value;
}

/**
 * Retrieve a server-to-server authentication key by server name.
 *
 * The caller must free the returned string.
 *
 * @return      The allocated key, or NULL when unavailable.
 */
char* get_server_auth_key(const char* server_name) {
    if (!session_is_ready() || !server_name) return NULL;

    char redis_key[128];
    snprintf(redis_key, sizeof(redis_key), AUTH_KEY_FMT, server_name);
    return fetch_key(redis_key);
}

/**
 * Compare a provided server key with the current or previous key for a world.
 *
 * Constant-time against both, so a failed match leaks nothing about how long a
 * prefix of the guess was correct.
 *
 * @return      Nonzero for a match against either key, otherwise zero.
 */
int validate_server_auth_key(const char* provided_key, const char* world_name) {
    if (!provided_key || !world_name) return 0;

    char redis_key[160];
    snprintf(redis_key, sizeof(redis_key), AUTH_KEY_FMT, world_name);
    char* current = fetch_key(redis_key);

    snprintf(redis_key, sizeof(redis_key), AUTH_KEY_PREVIOUS_FMT, world_name);
    char* previous = fetch_key(redis_key);

    if (!current && !previous) {
        /* Loud, and named: this is how a world silently disappears from the
         * list when a rotation is missed, and the failure it produces
         * downstream ("Server key unavailable") says nothing about Redis. */
        LOG_ERROR("No server auth key for '%s' in Redis. Rotate keys with "
                  "common/server_keys/generate_daily_server_keys.sh; until then "
                  "this world will refuse every realm handshake.", world_name);
        return 0;
    }

    int valid = 0;
    if (current  && auth_key_equal(provided_key, current))  valid = 1;
    if (previous && auth_key_equal(provided_key, previous)) {
        valid = 1;
        LOG_WARN("Realm authenticated to '%s' with the previous server key. "
                 "This is the rotation overlap; it expires shortly.", world_name);
    }

    free(current);
    free(previous);
    return valid;
}
