/**
 * @file
 * Store sessions, authentication tokens, and game tickets in Redis.
 */
#define _POSIX_C_SOURCE 200809L

#include "session.h"
#include "types.h"
#include "log.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <fcntl.h>
#include <sys/random.h>
#include <unistd.h>
#include <hiredis/hiredis.h>

/* --- The Redis connection pool -------------------------------------------
 *
 * This was one connection behind one process-wide mutex. Every session
 * validate, ticket mint, ticket consume and token consume in the whole service
 * queued on it, so the busiest moment the system has -- everybody logging in at
 * once -- was also the moment it serialized itself hardest. A pool makes those
 * concurrent; nothing else about how the calls are written changes.
 *
 * A connection is leased to a thread for the span the old lock was held, and
 * the lease is re-entrant: validate_session_ip_locked() runs inside another
 * command's span, and counting the depth is what keeps that from deadlocking
 * against itself.
 */

/** One pooled connection and its own reconnect backoff. */
typedef struct {
    redisContext* ctx;
    double        next_reconnect;   /**< CLOCK_MONOTONIC seconds. */
    int           in_use;
} RedisSlot;

static RedisSlot*      g_pool       = NULL;
static size_t          g_pool_size  = 0;
static pthread_mutex_t g_pool_lock  = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_pool_free  = PTHREAD_COND_INITIALIZER;

/** The connection this thread currently holds, and how deeply it is nested. */
static __thread RedisSlot* t_lease = NULL;
static __thread int        t_depth = 0;

/** Connections opened by default. Overridable with $MMO_REDIS_POOL_SIZE. */
#define REDIS_POOL_SIZE_DEFAULT 8

/** Seconds a caller waits for a free connection before giving up.
 *
 * Bounded rather than indefinite for the same reason the command timeout is:
 * a request that waits forever is a worker that never comes back.
 */
#define REDIS_LEASE_TIMEOUT_SECONDS 5

/** Cap how long a worker may block on one Redis command.
 *
 * Without a timeout a hung or unreachable Redis parks a worker in a blocking
 * socket read; with a pool that is one worker rather than all of them, but it
 * is still a worker that has to come back.
 */
#define REDIS_CONNECT_TIMEOUT_MS 2000
#define REDIS_COMMAND_TIMEOUT_MS 2000

/** Space out reconnect attempts so an outage does not turn every request into a
 *  blocking connect. */
#define REDIS_RECONNECT_BACKOFF_SECONDS 1.0

static const struct timeval k_redis_timeout = {
    .tv_sec  = REDIS_COMMAND_TIMEOUT_MS / 1000,
    .tv_usec = (REDIS_COMMAND_TIMEOUT_MS % 1000) * 1000,
};

/** Endpoint every pooled connection dials, resolved once at session_init. */
static char g_redis_host[256] = "127.0.0.1";

/** Redis credentials, from $MMO_REDIS_PASSWORD and $MMO_REDIS_USER.
 *
 * Redis holds every session key, every single-use auth token, every world
 * ticket and every server-to-server key this system has. With no AUTH and no
 * ACL, anything that can reach port 6379 owns every account -- and nothing in
 * the code or the setup ever asked for one.
 *
 * The password is optional at the code level, because an operator running on a
 * loopback-bound Redis inside one host is entitled to that choice, but startup
 * says plainly when it is missing rather than being quiet about it.
 *
 * $MMO_REDIS_USER selects an ACL user (Redis 6+); left unset, AUTH is issued
 * in its one-argument form against the legacy `requirepass`.
 */
static char g_redis_password[256] = {0};
static char g_redis_user[128]     = {0};
static int  g_redis_port      = 6379;

/**
 * Read a monotonic clock in seconds.
 */
static double redis_now_monotonic(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/** Report whether the session system has a usable pool. */
int session_is_ready(void) {
    return g_pool != NULL && g_pool_size > 0;
}

/** Connections in the pool. For tests and metrics. */
size_t redis_pool_size(void) {
    return g_pool_size;
}

/**
 * Lease one pooled connection to this thread.
 *
 * Re-entrant: a nested acquire keeps the connection already leased, so a helper
 * that issues its own commands inside a caller's span works without a second
 * connection and without deadlocking.
 *
 * Every acquire must be paired with redis_pool_release(), including when this
 * returns 0 -- the depth is counted either way.
 *
 * @return 1 when a connection is held, or 0 when none could be leased.
 */
int redis_pool_acquire(void) {
    if (t_depth++ > 0) return t_lease != NULL;

    if (!session_is_ready()) { t_lease = NULL; return 0; }

    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += REDIS_LEASE_TIMEOUT_SECONDS;

    pthread_mutex_lock(&g_pool_lock);
    for (;;) {
        for (size_t i = 0; i < g_pool_size; i++) {
            if (g_pool[i].in_use) continue;
            g_pool[i].in_use = 1;
            t_lease = &g_pool[i];
            pthread_mutex_unlock(&g_pool_lock);
            return 1;
        }

        if (pthread_cond_timedwait(&g_pool_free, &g_pool_lock, &deadline) == ETIMEDOUT) {
            pthread_mutex_unlock(&g_pool_lock);
            LOG_ERROR("Redis pool exhausted: no connection within %ds "
                              "(pool size %zu; raise MMO_REDIS_POOL_SIZE)",
                      REDIS_LEASE_TIMEOUT_SECONDS, g_pool_size);
            t_lease = NULL;
            return 0;
        }
    }
}

/** Return this thread's leased connection to the pool. */
void redis_pool_release(void) {
    if (t_depth <= 0) return;
    if (--t_depth > 0) return;

    if (!t_lease) return;

    pthread_mutex_lock(&g_pool_lock);
    t_lease->in_use = 0;
    pthread_cond_signal(&g_pool_free);
    pthread_mutex_unlock(&g_pool_lock);
    t_lease = NULL;
}

/**
 * Restore a leased connection to a usable state after a connection error.
 *
 * A redisContext is sticky: once its err field is set every later redisCommand on it
 * returns NULL, so a Redis restart, failover, or idle disconnect would otherwise
 * disable logins until the process was restarted. The errored context is deliberately
 * kept rather than freed, so redisReconnect can reuse its endpoint.
 *
 * @return Nonzero when the context is ready to carry a command.
 */
/**
 * Authenticate one freshly opened connection.
 *
 * Called on every path that produces a usable context -- the startup pool, the
 * lazy open, and the reconnect -- because a reconnected context is a new
 * connection as far as the server is concerned and arrives unauthenticated.
 * Missing that is the classic way an AUTH-protected deployment works until the
 * first Redis restart and then fails in a way nobody can reproduce.
 *
 * @return 1 when the connection may be used, or 0 when it must be discarded.
 */
static int redis_authenticate(redisContext* ctx) {
    if (!ctx || !g_redis_password[0]) return 1;

    redisReply* reply = g_redis_user[0]
        ? (redisReply*)redisCommand(ctx, "AUTH %s %s", g_redis_user, g_redis_password)
        : (redisReply*)redisCommand(ctx, "AUTH %s", g_redis_password);

    if (!reply) {
        LOG_ERROR("Redis AUTH failed: %s", ctx->errstr);
        return 0;
    }

    int ok = (reply->type != REDIS_REPLY_ERROR);
    if (!ok) {
        /* The error text is Redis's own and names no secret. Logged because
         * the alternative -- a connection that silently does nothing useful --
         * looks exactly like Redis being down. */
        LOG_ERROR("Redis AUTH rejected: %s", reply->str ? reply->str : "unknown");
    }
    freeReplyObject(reply);
    return ok;
}

static int redis_heal_leased(RedisSlot* slot) {
    if (!slot) return 0;

    if (!slot->ctx) {
        /* Lost to a failed reconnect, or never opened because Redis was down
         * when the pool was built. Open it now rather than retiring the slot. */
        double now = redis_now_monotonic();
        if (now < slot->next_reconnect) return 0;
        slot->next_reconnect = now + REDIS_RECONNECT_BACKOFF_SECONDS;

        const struct timeval connect_timeout = {
            .tv_sec  = REDIS_CONNECT_TIMEOUT_MS / 1000,
            .tv_usec = (REDIS_CONNECT_TIMEOUT_MS % 1000) * 1000,
        };
        slot->ctx = redisConnectWithTimeout(g_redis_host, g_redis_port, connect_timeout);
        if (slot->ctx && !slot->ctx->err && redis_authenticate(slot->ctx)) {
            redisSetTimeout(slot->ctx, k_redis_timeout);
            return 1;
        }
        if (slot->ctx) { redisFree(slot->ctx); slot->ctx = NULL; }
        return 0;
    }

    if (!slot->ctx->err) return 1;

    double now = redis_now_monotonic();
    if (now < slot->next_reconnect) return 0;
    slot->next_reconnect = now + REDIS_RECONNECT_BACKOFF_SECONDS;

    if (redisReconnect(slot->ctx) == REDIS_OK && !slot->ctx->err) {
        if (!redis_authenticate(slot->ctx)) return 0;
        redisSetTimeout(slot->ctx, k_redis_timeout);
        LOG_ERROR("Redis reconnected");
        return 1;
    }

    LOG_ERROR("Redis reconnect failed: %s", slot->ctx->errstr);
    return 0;
}

/**
 * Issue a Redis command on this thread's leased connection, healing a broken one.
 *
 * A NULL reply leaves the context permanently errored, so the connection is rebuilt
 * and the command retried once. Every command here is idempotent under a repeat
 * except the single-use ticket and token consumes; for those a retry that lands after
 * a successful-but-unread delete rejects one login, which the client recovers from by
 * requesting a new ticket.
 *
 * The caller must hold a lease from redis_pool_acquire().
 *
 * @return A reply the caller must free, or NULL when Redis is unreachable.
 */
redisReply* redis_command_locked(const char* format, ...) {
    RedisSlot* slot = t_lease;
    if (!redis_heal_leased(slot)) return NULL;

    va_list ap;
    va_start(ap, format);
    redisReply* reply = (redisReply*)redisvCommand(slot->ctx, format, ap);
    va_end(ap);
    if (reply) return reply;

    // An in-flight failure justifies reconnecting now rather than after the backoff.
    slot->next_reconnect = 0.0;
    if (!redis_heal_leased(slot)) return NULL;

    va_start(ap, format);
    reply = (redisReply*)redisvCommand(slot->ctx, format, ap);
    va_end(ap);
    return reply;
}

/** Read the configured pool size. */
static size_t configured_pool_size(void) {
    const char* raw = getenv("MMO_REDIS_POOL_SIZE");
    if (!raw || !*raw) return REDIS_POOL_SIZE_DEFAULT;

    char* end = NULL;
    long parsed = strtol(raw, &end, 10);
    if (end == raw || *end || parsed < 1 || parsed > 4096) {
        LOG_ERROR("MMO_REDIS_POOL_SIZE='%s' is not a usable pool size; "
                          "using %d", raw, REDIS_POOL_SIZE_DEFAULT);
        return REDIS_POOL_SIZE_DEFAULT;
    }
    return (size_t)parsed;
}

/**
 * Connect the shared session context to the configured Redis endpoint.
 *
 * @return      Nonzero when connected, otherwise zero.
 */
int session_init(void) {
    const char* redis_host = getenv("REDIS_HOST");
    if (!redis_host || !*redis_host) redis_host = "127.0.0.1";
    snprintf(g_redis_host, sizeof(g_redis_host), "%s", redis_host);

    const char* redis_password = getenv("MMO_REDIS_PASSWORD");
    snprintf(g_redis_password, sizeof(g_redis_password), "%s",
             redis_password ? redis_password : "");

    const char* redis_user = getenv("MMO_REDIS_USER");
    snprintf(g_redis_user, sizeof(g_redis_user), "%s",
             redis_user ? redis_user : "");

    if (!g_redis_password[0]) {
        /* Deliberately loud, and deliberately not fatal.
         *
         * Everything that authenticates a player or a server lives in this
         * Redis: session keys, single-use login tokens, world-entry tickets
         * and the server-to-server keys. An unauthenticated instance reachable
         * beyond the host it runs on is a complete compromise of every
         * account, and the failure is entirely silent until it is exploited.
         *
         * Refusing to start would strand a working single-host development
         * setup, so this warns instead -- but it warns every time, with the
         * fix in the message. */
        LOG_WARN("Redis has no password configured (MMO_REDIS_PASSWORD unset). "
                 "It holds session keys, login tokens, world tickets and "
                 "server keys; anything that can reach %s:%d owns every "
                 "account. Set requirepass (or an ACL user) and bind Redis to "
                 "an interface players cannot reach.",
                 g_redis_host, g_redis_port);
    }

    const char* redis_port_str = getenv("REDIS_PORT");
    g_redis_port = redis_port_str ? atoi(redis_port_str) : 6379;
    if (g_redis_port <= 0 || g_redis_port > 65535) g_redis_port = 6379;

    size_t wanted = configured_pool_size();

    pthread_mutex_lock(&g_pool_lock);
    g_pool = calloc(wanted, sizeof(*g_pool));
    if (!g_pool) {
        pthread_mutex_unlock(&g_pool_lock);
        LOG_ERROR("Redis pool: out of memory for %zu connections", wanted);
        return 0;
    }
    g_pool_size = wanted;
    pthread_mutex_unlock(&g_pool_lock);

    const struct timeval connect_timeout = {
        .tv_sec  = REDIS_CONNECT_TIMEOUT_MS / 1000,
        .tv_usec = (REDIS_CONNECT_TIMEOUT_MS % 1000) * 1000,
    };

    /* Open every connection up front so a Redis that is down is reported here,
     * at startup, rather than as a puzzling authentication failure later. The
     * first connection is required; the rest are opened lazily on first use if
     * they fail now, because a partially available pool still serves players. */
    size_t opened = 0;
    for (size_t i = 0; i < g_pool_size; i++) {
        g_pool[i].ctx = redisConnectWithTimeout(g_redis_host, g_redis_port, connect_timeout);
        if (g_pool[i].ctx && !g_pool[i].ctx->err && redis_authenticate(g_pool[i].ctx)) {
            redisSetTimeout(g_pool[i].ctx, k_redis_timeout);
            opened++;
            continue;
        }
        if (g_pool[i].ctx) {
            if (i == 0)
                LOG_ERROR("Redis connection error: %s", g_pool[i].ctx->errstr);
            redisFree(g_pool[i].ctx);
            g_pool[i].ctx = NULL;
        } else if (i == 0) {
            LOG_ERROR("Redis connection error: can't allocate context");
        }
    }

    if (opened == 0) {
        free(g_pool);
        g_pool = NULL;
        g_pool_size = 0;
        return 0;
    }

    LOG_INFO("Session system initialized (Redis: %s:%d, %zu/%zu connections open)",
             g_redis_host, g_redis_port, opened, g_pool_size);
    return 1;
}

/**
 * Fill a buffer from the kernel CSPRNG, retrying only interruptions.
 *
 * getrandom(2) rather than opening /dev/urandom: no descriptor to leak or run
 * out of, and no chance of reading from a /dev that a broken chroot has
 * replaced with something else. The realm's ticket generator made the same
 * move for the same reasons; see realm_server/src/world_list.c.
 *
 * @return 1 when the whole buffer was filled, otherwise 0.
 */
static int session_fill_random(unsigned char* out, size_t len) {
    size_t filled = 0;
    while (filled < len) {
        ssize_t got = getrandom(out + filled, len - filled, 0);
        if (got < 0) {
            if (errno == EINTR) continue;
            return 0;
        }
        if (got == 0) return 0;
        filled += (size_t)got;
    }
    return 1;
}

/**
 * Generate and store a binary-safe account session with an expiry.
 *
 * @param ip_address      Source address to record, or NULL to store an unknown address.
 * @param out_session_key Receives exactly 32 key bytes and is not terminated.
 * @return                Nonzero on success, otherwise zero.
 */
int session_create(uint32_t account_id, const char* ip_address, char* out_session_key) {
    if (!session_is_ready() || !out_session_key) {
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
    
    /* The correlation id for this whole login, minted here because this is the
     * first moment the player exists as a session. It goes into the hash so
     * the realm can adopt it, and from there into the world ticket, and it is
     * adopted by this thread immediately so the rest of the login logs under
     * it too. */
    char trace_id[TRACE_ID_LEN];
    log_new_trace(trace_id);
    log_set_trace(trace_id);

    redis_pool_acquire();

    // Use %b for binary-safe session key storage (exactly 32 bytes)
    redisReply* reply = redis_command_locked(
        "HMSET %s session_key %b created_at %ld expires_at %ld ip_address %s trace_id %s",
        redis_key, key_buffer, (size_t)32, now, expires_at,
        ip_address ? ip_address : "unknown", trace_id
    );
    
    if (!reply || reply->type == REDIS_REPLY_ERROR) {
        if (reply) {
            LOG_ERROR("Redis error in session_create: %s", reply->str);
            freeReplyObject(reply);
        }
        redis_pool_release();
        free(session_key);
        return 0;
    }
    freeReplyObject(reply);
    
    reply = redis_command_locked("EXPIRE %s %d", redis_key, SESSION_EXPIRY_SECONDS);
    if (reply) freeReplyObject(reply);
    
    redis_pool_release();
    
    memcpy(out_session_key, key_buffer, 32);
    
    
    free(session_key);
    return 1;
}

int session_trace_id(uint32_t account_id, char* out, size_t out_size) {
    if (!out || out_size == 0) return 0;
    out[0] = '\0';
    if (!session_is_ready()) return 0;

    char redis_key[64];
    snprintf(redis_key, sizeof(redis_key), "session:%u", account_id);

    redis_pool_acquire();
    redisReply* reply = redis_command_locked("HGET %s trace_id", redis_key);
    redis_pool_release();

    int found = 0;
    if (reply && reply->type == REDIS_REPLY_STRING && reply->len > 0) {
        snprintf(out, out_size, "%s", reply->str);
        found = 1;
    }
    if (reply) freeReplyObject(reply);
    return found;
}

/**
 * Compare two byte ranges in time independent of where they first differ.
 *
 * memcmp returns at the first differing byte, so its timing describes how long
 * a prefix of a guess was correct -- a usable oracle against a secret that can
 * be submitted repeatedly.
 *
 * @return Nonzero when the ranges are equal.
 */
static int constant_time_equal(const void* a, const void* b, size_t len) {
    const unsigned char* x = (const unsigned char*)a;
    const unsigned char* y = (const unsigned char*)b;
    unsigned char diff = 0;
    for (size_t i = 0; i < len; i++) diff |= (unsigned char)(x[i] ^ y[i]);
    return diff == 0;
}

/**
 * Read the address recorded when a session was created.
 *
 * Caller must hold a Redis lease from redis_pool_acquire().
 *
 * @return 1 when a usable address was copied, 0 when the field is absent or
 *         recorded as unknown.
 */
static int session_stored_ip_locked(const char* redis_key, char* out, size_t out_size) {
    redisReply* reply = redis_command_locked("HGET %s ip_address", redis_key);
    int usable = 0;

    if (reply && reply->type == REDIS_REPLY_STRING &&
        reply->len > 0 && strcmp(reply->str, "unknown") != 0) {
        size_t copy = reply->len < out_size - 1 ? (size_t)reply->len : out_size - 1;
        memcpy(out, reply->str, copy);
        out[copy] = '\0';
        usable = 1;
    }

    if (reply) freeReplyObject(reply);
    return usable;
}

/**
 * Validate a 32-byte account session and refresh its Redis expiry.
 *
 * @return      Nonzero for a matching unexpired session, otherwise zero.
 */
int session_validate(uint32_t account_id, const char* session_key) {
    return session_validate_from(account_id, session_key, NULL);
}

/**
 * Validate a 32-byte account session, optionally bound to a source address.
 *
 * A session is a Redis hash written by session_create() or session_store().
 * There is exactly one accepted shape and three things that must all hold: the
 * hash exists, its session_key field is 32 bytes equal to the one presented,
 * and its expires_at field is in the future. Anything else is a refusal.
 *
 * Two older concessions are gone from this function, because each of them was
 * a way to hold a session key and skip the expiry check:
 *
 *   - A plain-string value read with GET, kept "for backward compatibility
 *     with session_store". session_store() has written a hash for as long as
 *     this fallback has existed, so nothing could produce the shape it
 *     accepted -- but the branch validated the key and returned without ever
 *     looking at an expiry, because a plain string has no fields to look at.
 *   - A hash with no expires_at field, logged as "assuming valid". A session
 *     that cannot say when it ends does not get to be treated as one that
 *     has not ended yet.
 *
 * @return      Nonzero for a matching unexpired session, otherwise zero.
 */
int session_validate_from(uint32_t account_id, const char* session_key,
                          const char* peer_ip) {
    if (!session_is_ready() || !session_key) {
        LOG_WARN("Session validate: Redis is %s and the key is %s",
                 session_is_ready() ? "ready" : "unavailable",
                 session_key ? "present" : "missing");
        return 0;
    }
    
    char redis_key[64];
    snprintf(redis_key, sizeof(redis_key), "session:%u", account_id);
    
    redis_pool_acquire();
    
    // First check if key exists
    redisReply* exists_reply = redis_command_locked("EXISTS %s", redis_key);
    if (!exists_reply || exists_reply->type != REDIS_REPLY_INTEGER || exists_reply->integer == 0) {
        LOG_DEBUG("Session validate: Key '%s' does not exist in Redis", redis_key);
        if (exists_reply) freeReplyObject(exists_reply);
        redis_pool_release();
        return 0;
    }
    freeReplyObject(exists_reply);
    
    // Get the session_key field from hash
    redisReply* reply = redis_command_locked("HGET %s session_key", redis_key);
    
    if (!reply) {
        LOG_DEBUG("Session validate: Redis command failed for HGET %s session_key", redis_key);
        redis_pool_release();
        return 0;
    }
    
    if (reply->type != REDIS_REPLY_STRING) {
        LOG_WARN("Session validate: 'session:%u' has no session_key field "
                 "(reply type %d) — refusing", account_id, reply->type);
        freeReplyObject(reply);
        redis_pool_release();
        return 0;
    }

    /* Exactly 32 bytes, not at least 32.
     *
     * The comparison itself only ever reads SESSION_KEY_LENGTH bytes, so a
     * >= test admitted any longer stored value whose first 32 bytes matched
     * -- a stored key and a stored key with anything appended validated
     * against the same presented credential. Nothing writes a longer value,
     * which is exactly why a longer one appearing should be a refusal. */
    size_t stored_len = reply->len;
    int valid = (stored_len == SESSION_KEY_LENGTH &&
                 constant_time_equal(reply->str, session_key, SESSION_KEY_LENGTH));

    freeReplyObject(reply);

    if (!valid) {
        LOG_DEBUG("Session validate: Key mismatch!");
        redis_pool_release();
        return 0;
    }

    /* The key is right; require it to be presented from where it was issued.
     *
     * The realm link is plaintext and carries this key in every connect packet,
     * so a passive observer has it. Binding it to the address the login server
     * recorded means the captured key is not on its own enough to open a realm
     * session from anywhere else. */
    if (peer_ip && peer_ip[0]) {
        char issued_to[64] = {0};
        if (session_stored_ip_locked(redis_key, issued_to, sizeof(issued_to)) &&
            strcmp(issued_to, peer_ip) != 0) {
            LOG_WARN("Session validate: account %u presented its key from %s "
                     "but it was issued to %s — refusing",
                     account_id, peer_ip, issued_to);
            redis_pool_release();
            return 0;
        }
    }
    
    /* Check expiry. A session that cannot say when it ends is refused, not
     * assumed live: every writer sets expires_at, so its absence means the
     * hash was not written by this code, and the one thing an attacker who
     * could write a partial hash would want is for the missing field to be
     * read as "no expiry". */
    reply = redis_command_locked("HGET %s expires_at", redis_key);
    if (!reply || reply->type != REDIS_REPLY_STRING) {
        LOG_WARN("Session validate: 'session:%u' has no expires_at field — refusing",
                 account_id);
        if (reply) freeReplyObject(reply);
        redis_pool_release();
        return 0;
    }

    time_t expires_at = atol(reply->str);
    freeReplyObject(reply);

    time_t now = time(NULL);
    if (now > expires_at) {
        LOG_DEBUG("Session validate: Session expired (now=%ld, expires=%ld)", now, expires_at);
        redisReply* del_reply = redis_command_locked("DEL %s", redis_key);
        if (del_reply) freeReplyObject(del_reply);
        redis_pool_release();
        return 0;
    }

    // Refresh expiry
    reply = redis_command_locked("EXPIRE %s %d", redis_key, SESSION_EXPIRY_SECONDS);
    if (reply) freeReplyObject(reply);
    
    redis_pool_release();
    
    LOG_DEBUG("Session validated successfully for account %u (hash format with expiry)", account_id);
    return 1;
}


/** Close every pooled Redis connection.
 *
 * Callers must have stopped every thread that can take a lease; this frees the
 * table the leases point into.
 */
void session_close(void) {
    pthread_mutex_lock(&g_pool_lock);
    if (!g_pool) {
        pthread_mutex_unlock(&g_pool_lock);
        return;
    }

    for (size_t i = 0; i < g_pool_size; i++) {
        if (g_pool[i].ctx) {
            redisFree(g_pool[i].ctx);
            g_pool[i].ctx = NULL;
        }
    }
    free(g_pool);
    g_pool      = NULL;
    g_pool_size = 0;
    pthread_mutex_unlock(&g_pool_lock);

    LOG_INFO("Session system closed");
}

/**
 * Store a supplied 32-byte player session in the hash format used by validation.
 *
 * @return      Nonzero on success, otherwise zero.
 */
int session_store(uint32_t player_id, const char* session_key) {
    if (!session_is_ready() || !session_key || player_id == 0) {
        return 0;
    }
    
    char key[64];
    snprintf(key, sizeof(key), "session:%u", player_id);
    
    time_t now = time(NULL);
    time_t expires_at = now + SESSION_EXPIRY_SECONDS;
    
    // Create a temporary buffer with EXACTLY 32 bytes (no null terminator)
    char key_buffer[32];
    memcpy(key_buffer, session_key, 32);
    
    redis_pool_acquire();
    
    // Use HMSET format with binary-safe key storage (exactly 32 bytes)
    // Format the session_key as a binary-safe string by using %b with explicit length
    redisReply *reply = redis_command_locked(
        "HMSET %s session_key %b created_at %ld expires_at %ld",
        key, key_buffer, (size_t)32, now, expires_at);
    
    if (!reply || reply->type == REDIS_REPLY_ERROR) {
        if (reply) {
            LOG_ERROR("Redis error in session_store: %s", reply->str);
            freeReplyObject(reply);
        }
        redis_pool_release();
        LOG_ERROR("Failed to store session for player %u", player_id);
        return 0;
    }
    freeReplyObject(reply);
    
    // Set expiry on the hash
    reply = redis_command_locked("EXPIRE %s %d", key, SESSION_EXPIRY_SECONDS);
    if (reply) freeReplyObject(reply);
    
    redis_pool_release();
    
    LOG_DEBUG("Stored session for player %u using HMSET format (32 bytes)", player_id);
    return 1;
}

/* Deliberately absent: session_mark_active(), session_refresh() and
 * session_invalidate().
 *
 * All three were defined and exported and never called by anything in any of
 * the three services. session_mark_active() wrote a second key,
 * "session:%u:active", that nothing ever read; session_refresh() re-set an
 * expiry that session_validate() already re-sets on every use;
 * session_invalidate() was session_remove() with a return value.
 *
 * They are gone rather than kept "in case": an exported function that is never
 * called still has to be read, kept compiling, and reasoned about by anyone
 * auditing what touches a session -- and the one that wrote a key nothing read
 * was actively misleading about what a session consists of.
 *
 * session_remove() below no longer deletes the :active key, because nothing
 * writes one any more.
 */

/** Delete a player's session. */
void session_remove(uint32_t player_id) {
    if (!session_is_ready() || player_id == 0) {
        return;
    }

    char session_key[64];
    snprintf(session_key, sizeof(session_key), "session:%u", player_id);

    redis_pool_acquire();
    redisReply *reply = redis_command_locked("DEL %s", session_key);

    if (reply) freeReplyObject(reply);
    redis_pool_release();
}

/**
 * Generate a random 32-character alphanumeric session key.
 *
 * The caller must free the returned buffer.
 *
 * @return      The allocated terminated key, or NULL when allocation or random input fails.
 */
char* generate_session_key(void) {
    const char charset[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    const unsigned charset_size = 62;

    /* Rejection sampling, not `byte % 62`.
     *
     * 256 is not a multiple of 62: 256 = 4*62 + 8, so bytes 0..247 map four
     * ways onto each character while 248..255 give the first eight characters
     * a fifth. Those eight characters ('a'..'h') came up 5/256 of the time and
     * the other fifty-four came up 4/256 -- a 25% bias on every position of
     * what is meant to be a uniform 32-character credential. Over 32
     * positions that is a real, if modest, reduction in the work of guessing
     * one. Discarding the eight bytes above the largest multiple of 62 costs
     * an expected 3% extra entropy and removes the bias exactly.
     *
     * The refill is deliberate rather than one byte at a time: a rejected byte
     * is cheap, a syscall per rejection is not. */
    const unsigned reject_at = 256u - (256u % charset_size);   /* 248 */

    char* key = malloc(33);
    if (!key) return NULL;

    size_t produced = 0;
    while (produced < 32) {
        unsigned char batch[64];
        if (!session_fill_random(batch, sizeof(batch))) {
            free(key);
            return NULL;
        }
        for (size_t i = 0; i < sizeof(batch) && produced < 32; i++) {
            if (batch[i] >= reject_at) continue;   /* would bias the low letters */
            key[produced++] = charset[batch[i] % charset_size];
        }
    }
    key[32] = '\0';

    return key;
}

/**
 * Record that a character is live in a world, or refresh an existing mark.
 *
 * @return 1 when Redis accepted the mark, otherwise 0.
 */
int world_session_mark(uint32_t character_id, uint32_t world_id) {
    if (!session_is_ready() || character_id == 0) return 0;

    char key[64];
    snprintf(key, sizeof(key), "world_session:%u", character_id);

    redis_pool_acquire();
    redisReply* reply = redis_command_locked("SETEX %s %d %u",
                                             key, WORLD_SESSION_TTL_SECONDS, world_id);
    int ok = (reply && reply->type == REDIS_REPLY_STATUS &&
              strcmp(reply->str, "OK") == 0);
    if (reply) freeReplyObject(reply);
    redis_pool_release();
    return ok;
}

/** Clear a character's world-session mark. */
void world_session_clear(uint32_t character_id) {
    if (!session_is_ready() || character_id == 0) return;

    char key[64];
    snprintf(key, sizeof(key), "world_session:%u", character_id);

    redis_pool_acquire();
    redisReply* reply = redis_command_locked("DEL %s", key);
    if (reply) freeReplyObject(reply);
    redis_pool_release();
}

/**
 * Report which world a character is currently live in.
 *
 * @return The world identifier, or 0 when no live session is recorded.
 */
uint32_t world_session_world(uint32_t character_id) {
    if (!session_is_ready() || character_id == 0) return 0;

    char key[64];
    snprintf(key, sizeof(key), "world_session:%u", character_id);

    redis_pool_acquire();
    redisReply* reply = redis_command_locked("GET %s", key);
    uint32_t world_id = 0;
    if (reply && reply->type == REDIS_REPLY_STRING)
        world_id = (uint32_t)strtoul(reply->str, NULL, 10);
    if (reply) freeReplyObject(reply);
    redis_pool_release();
    return world_id;
}

/**
 * Store a game-ticket value with a caller-selected Redis expiry.
 *
 * @return      Nonzero when Redis accepts the value, otherwise zero.
 */
int store_game_ticket_in_redis(const char* key, const char* value, int expiry_seconds) {
    if (!session_is_ready()) return 0;
    
    redis_pool_acquire();
    redisReply* reply = redis_command_locked("SETEX %s %d %s", 
                                     key, expiry_seconds, value);
    if (!reply) {
        redis_pool_release();
        return 0;
    }
    
    int success = (reply->type == REDIS_REPLY_STATUS && 
                   strcmp(reply->str, "OK") == 0);
    freeReplyObject(reply);
    redis_pool_release();
    return success;
}

/**
 * Store a single-use authentication token with its fixed 60-second lifetime.
 *
 * @return      Nonzero when Redis accepts the token, otherwise zero.
 */
int auth_token_store(const char* token, uint32_t player_id) {
    if (!session_is_ready() || !token) return 0;

    char key[80];
    snprintf(key, sizeof(key), "auth_token:%.32s", token);

    char value[32];
    snprintf(value, sizeof(value), "%u", player_id);

    redis_pool_acquire();
    redisReply* reply = redis_command_locked("SETEX %s 60 %s", key, value);
    int ok = (reply && reply->type == REDIS_REPLY_STATUS &&
              strcmp(reply->str, "OK") == 0);
    if (reply) freeReplyObject(reply);
    redis_pool_release();

    return ok;
}

/**
 * Atomically fetch and delete a single-use authentication token.
 *
 * @return      The associated player identifier, or zero when the token is absent or invalid.
 */
uint32_t auth_token_consume(const char* token) {
    if (!session_is_ready() || !token) return 0;

    char key[80];
    snprintf(key, sizeof(key), "auth_token:%.32s", token);

    redis_pool_acquire();

    // Fetch and delete atomically. A mutex alone is insufficient when more
    // than one login-server process shares Redis.
    static const char consume_script[] =
        "local v=redis.call('GET',KEYS[1]); "
        "if v then redis.call('DEL',KEYS[1]) end; return v";
    redisReply* reply = redis_command_locked("EVAL %b 1 %s",
                                     consume_script, strlen(consume_script), key);
    if (!reply || reply->type != REDIS_REPLY_STRING) {
        if (reply) freeReplyObject(reply);
        redis_pool_release();
        return 0;
    }

    uint32_t player_id = (uint32_t)strtoul(reply->str, NULL, 10);
    freeReplyObject(reply);

    redis_pool_release();

    return player_id;
}

/**
 * Validate and consume a realm-issued game ticket.
 *
 * Ticket values use the character_id:world_id:account_id:client_ip text layout.
 * The trailing address is the peer the realm issued the ticket to; a ticket
 * presented from anywhere else is refused. The world link is plaintext, so the
 * ticket is visible to anyone on the path, and it is worth an account takeover
 * to whoever captures it before the legitimate client redeems it.
 *
 * @return      Nonzero when the ticket is parsed and consumed, otherwise zero.
 */
int validate_game_ticket(const char* ticket, const char* peer_ip,
                         uint32_t expected_world_id,
                         uint32_t* out_account_id, uint32_t* out_character_id,
                         uint32_t* out_world_id) {
    if (!session_is_ready() || !ticket) return 0;
    
    char ticket_key[128];
    snprintf(ticket_key, sizeof(ticket_key), "ticket:%s", ticket);
    
    redis_pool_acquire();

    /* Fetch and delete in one server-side step, the same way auth_token_consume does.
     * A separate GET then DEL leaves a window in which two connections replaying the
     * same ticket both read it before either deletes; because session_registry_add
     * kicks the older session, the replayer would win and boot the real player. Only
     * the caller whose script actually removed the key gets a value back.
     *
     * EVAL rather than GETDEL so this does not require Redis 6.2. */
    static const char consume_script[] =
        "local v=redis.call('GET',KEYS[1]); "
        "if v then redis.call('DEL',KEYS[1]) end; return v";
    redisReply* reply = redis_command_locked("EVAL %b 1 %s",
                                     consume_script, strlen(consume_script), ticket_key);
    
    if (!reply || reply->type != REDIS_REPLY_STRING) {
        if (reply) freeReplyObject(reply);
        redis_pool_release();
        return 0;
    }
    
    /* Stored by the realm as:
     *   "character_id:world_id:account_id:client_ip:trace_id"
     *
     * The trace id is the fifth field and is optional: a ticket minted by a
     * realm from before correlation existed has four, and refusing it would
     * turn an upgrade into an outage for every player mid-login. A missing
     * trace id costs a correlated log line, not a session. */
    uint32_t char_id = 0, world_id = 0, account_id = 0;
    char issued_to[64] = {0};
    char trace_id[TRACE_ID_LEN] = {0};
    int fields = sscanf(reply->str, "%u:%u:%u:%63[^:]:%16s",
                        &char_id, &world_id, &account_id, issued_to, trace_id);
    if (fields == 4) trace_id[0] = '\0';

    freeReplyObject(reply);
    redis_pool_release();

    /* At least four fields required. A three-field ticket is one this build's
     * realm did not mint; accepting it as "unbound" would reinstate exactly
     * the replay the address field exists to close. The fifth, the trace id,
     * is optional -- see above. Tickets live 60 seconds, so the only cost of
     * the strictness is that entries minted across an upgrade are
     * re-requested. */
    if (fields < 4) {
        LOG_ERROR("Rejecting game ticket with %d of at least 4 fields", fields);
        return 0;
    }

    /* Adopt the player's correlation id for the rest of this admission, so the
     * world's lines about them join the login's and the realm's. */
    if (trace_id[0]) log_set_trace(trace_id);

    if (peer_ip && peer_ip[0] && strcmp(issued_to, peer_ip) != 0) {
        LOG_ERROR("Game ticket for account %u was issued to %s but "
                          "presented from %s — refusing",
                  account_id, issued_to, peer_ip);
        return 0;
    }

    /* The world the realm minted this ticket for.
     *
     * The field was already in the ticket and already parsed here; the world
     * server simply threw the answer away, so any world accepted any world's
     * ticket. That let a client take a ticket the realm issued for one world
     * and open a session on another -- entering a character into a world whose
     * database has never heard of it, and past whatever the realm decided
     * about capacity, ownership or a world being offline. The ticket is
     * consumed by this point either way, so a mismatch costs the attacker the
     * ticket. */
    if (expected_world_id != 0 && world_id != expected_world_id) {
        LOG_ERROR("Game ticket for account %u was minted for world %u but "
                          "presented to world %u — refusing",
                  account_id, world_id, expected_world_id);
        return 0;
    }

    if (out_account_id)   *out_account_id = account_id;
    if (out_character_id)  *out_character_id = char_id;
    if (out_world_id)      *out_world_id = world_id;

    LOG_DEBUG("Game ticket validated: account=%u, char=%u, world=%u",
              account_id, char_id, world_id);
    return 1;
}
