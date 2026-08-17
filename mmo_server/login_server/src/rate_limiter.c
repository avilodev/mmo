/**
 * @file
 * Enforce bounded per-address authentication and connection rates for the login server.
 */

#include "rate_limiter.h"

#include "log.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <pthread.h>
#include <stdint.h>

#define RL_TABLE_BITS   12
#define RL_TABLE_SIZE   (1u << RL_TABLE_BITS)   // 4096 addresses tracked
#define RL_TABLE_MASK   (RL_TABLE_SIZE - 1u)
#define RL_MAX_PROBES   32                      // bounds worst-case lookup cost

#define RL_MAX_FAILS    5       // failures inside one window before a block
#define RL_WINDOW_SECS  60      // counting window
#define RL_BLOCK_SECS   300     // block duration

/** Maximum accepted connections per address during one connection window. */
#define RL_MAX_CONNS         30
#define RL_CONN_WINDOW_SECS  60

/** Track failure, connection, and blocking windows for one peer address. */
typedef struct {
    char ip[RL_IP_MAXLEN];  // empty string = slot never used
    int  fail_count;
    long window_start;      // when the current failure window began
    int  conn_count;
    long conn_window_start; // when the current connection window began
    long block_until;       // 0 = not blocked
    long last_seen;         // last time this entry was touched
} RateLimitEntry;

static RateLimitEntry  g_entries[RL_TABLE_SIZE];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

// Monotonic seconds. Wall-clock steps (NTP, manual clock changes) must not be
// able to shorten a block or reset a counting window.
static long rl_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec;
}

/** Compute the FNV-1a hash used to place a textual address. */
static uint32_t hash_ip(const char* ip) {
    uint32_t h = 2166136261u;                   // FNV-1a
    for (const unsigned char* p = (const unsigned char*)ip; *p; p++) {
        h ^= (uint32_t)*p;
        h *= 16777619u;
    }
    return h;
}

/** Report whether an occupied entry may be reclaimed. */
static int entry_is_dead(const RateLimitEntry* e, long now) {
    if (e->ip[0] == '\0')      return 0;   // never used, not "dead"
    if (e->block_until > now)  return 0;   // actively blocked, must keep
    return (now - e->last_seen) > RL_WINDOW_SECS;
}

/**
 * Find a live entry without creating one.
 *
 * @return      The matching entry, or NULL when the address is absent or stale.
 */
static RateLimitEntry* find(const char* ip, long now) {
    uint32_t h = hash_ip(ip);
    for (uint32_t i = 0; i < RL_MAX_PROBES; i++) {
        RateLimitEntry* e = &g_entries[(h + i) & RL_TABLE_MASK];
        if (e->ip[0] == '\0') return NULL;             // chain ends here
        if (strcmp(e->ip, ip) == 0)
            return entry_is_dead(e, now) ? NULL : e;   // stale entry reads as absent
    }
    return NULL;
}

/**
 * Find an address entry or claim an empty or expired probe slot.
 *
 * @return      The matching or initialized entry, or NULL when the bounded probe range is saturated.
 */
static RateLimitEntry* find_or_create(const char* ip, long now) {
    uint32_t h = hash_ip(ip);
    RateLimitEntry* reusable = NULL;

    for (uint32_t i = 0; i < RL_MAX_PROBES; i++) {
        RateLimitEntry* e = &g_entries[(h + i) & RL_TABLE_MASK];

        if (e->ip[0] == '\0') {
            // Virgin slot ends the chain. Reuse an earlier dead slot if we
            // passed one; placing the entry earlier keeps future probes short.
            RateLimitEntry* target = reusable ? reusable : e;
            memset(target, 0, sizeof(*target));
            strncpy(target->ip, ip, sizeof(target->ip) - 1);
            target->window_start      = now;
            target->conn_window_start = now;
            target->last_seen         = now;
            return target;
        }

        if (strcmp(e->ip, ip) == 0) {
            if (entry_is_dead(e, now)) {
                // Same address, but its history has lapsed. Start it fresh
                // rather than resurrecting a stale fail_count.
                memset(e, 0, sizeof(*e));
                strncpy(e->ip, ip, sizeof(e->ip) - 1);
                e->window_start      = now;
                e->conn_window_start = now;
            }
            e->last_seen = now;
            return e;
        }

        if (!reusable && entry_is_dead(e, now))
            reusable = e;
    }

    if (reusable) {
        memset(reusable, 0, sizeof(*reusable));
        strncpy(reusable->ip, ip, sizeof(reusable->ip) - 1);
        reusable->window_start      = now;
        reusable->conn_window_start = now;
        reusable->last_seen         = now;
        return reusable;
    }

    return NULL;   // saturated — caller fails closed
}

/** Clear all rate-limit entries under the shared limiter lock. */
void rate_limiter_init(void) {
    pthread_mutex_lock(&g_lock);
    memset(g_entries, 0, sizeof(g_entries));
    pthread_mutex_unlock(&g_lock);
}

/**
 * Test whether an address is blocked or cannot be identified.
 *
 * @return      Nonzero when the connection must be rejected, otherwise zero.
 */
int rate_limiter_check(const char* ip) {
    // No usable address means no way to police this connection.
    if (!ip || ip[0] == '\0') return 1;

    pthread_mutex_lock(&g_lock);
    long now = rl_now();
    RateLimitEntry* e = find(ip, now);
    int blocked = (e && e->block_until > now);
    if (e) e->last_seen = now;
    pthread_mutex_unlock(&g_lock);

    return blocked;
}

/**
 * Record an authentication failure and apply the failure threshold.
 *
 * @return      Nonzero when the connection must be dropped, otherwise zero.
 */
int rate_limiter_record_failure(const char* ip) {
    if (!ip || ip[0] == '\0') return 1;   // untrackable: fail closed

    pthread_mutex_lock(&g_lock);
    long now = rl_now();

    RateLimitEntry* e = find_or_create(ip, now);
    if (!e) {
        pthread_mutex_unlock(&g_lock);
        LOG_WARN_RL(5, 60,
                    "[RATE_LIMIT] table saturated, cannot track %s — dropping connection",
                    ip);
        return 1;   // fail closed
    }

    if (e->block_until > now) {
        pthread_mutex_unlock(&g_lock);
        return 1;   // already blocked, nothing more to count
    }

    if (now - e->window_start > RL_WINDOW_SECS) {
        e->fail_count   = 0;
        e->window_start = now;
    }

    e->fail_count++;

    int drop = 0;
    if (e->fail_count >= RL_MAX_FAILS) {
        e->block_until = now + RL_BLOCK_SECS;
        drop = 1;
    }
    int fails = e->fail_count;
    pthread_mutex_unlock(&g_lock);

    if (drop)
        LOG_WARN("[RATE_LIMIT] %s blocked for %ds (%d failures in %ds)",
                 ip, RL_BLOCK_SECS, fails, RL_WINDOW_SECS);

    return drop;
}

/**
 * Record an accepted connection and apply the connection-churn threshold.
 *
 * @return      Nonzero when the connection must be refused, otherwise zero.
 */
int rate_limiter_record_connection(const char* ip) {
    if (!ip || ip[0] == '\0') return 1;   // untrackable: fail closed

    pthread_mutex_lock(&g_lock);
    long now = rl_now();

    RateLimitEntry* e = find_or_create(ip, now);
    if (!e) {
        pthread_mutex_unlock(&g_lock);
        LOG_WARN_RL(5, 60,
                    "[RATE_LIMIT] table saturated, cannot track %s — refusing connection",
                    ip);
        return 1;   // fail closed
    }

    if (e->block_until > now) {
        pthread_mutex_unlock(&g_lock);
        return 1;
    }

    if (now - e->conn_window_start > RL_CONN_WINDOW_SECS) {
        e->conn_count        = 0;
        e->conn_window_start = now;
    }

    e->conn_count++;

    int refuse = 0;
    if (e->conn_count > RL_MAX_CONNS) {
        // Churning connections is as hostile as churning passwords, so it
        // earns the same block rather than a per-connection brush-off.
        e->block_until = now + RL_BLOCK_SECS;
        refuse = 1;
    }
    int conns = e->conn_count;
    pthread_mutex_unlock(&g_lock);

    if (refuse)
        LOG_WARN("[RATE_LIMIT] %s blocked for %ds (%d connections in %ds)",
                 ip, RL_BLOCK_SECS, conns, RL_CONN_WINDOW_SECS);

    return refuse;
}

/** Clear expired failure history after successful authentication. */
void rate_limiter_note_success(const char* ip) {
    if (!ip || ip[0] == '\0') return;

    pthread_mutex_lock(&g_lock);
    long now = rl_now();
    RateLimitEntry* e = find(ip, now);
    // A blocked address stays blocked; succeeding once does not buy a way out.
    if (e && e->block_until <= now) {
        e->fail_count   = 0;
        e->window_start = now;
        e->last_seen    = now;
    }
    pthread_mutex_unlock(&g_lock);
}

/**
 * Write a socket peer's IPv4 or IPv6 address into a caller buffer.
 *
 * Failure is reported by leaving the output as an empty string.
 */
void rate_limiter_peer_ip(int fd, char* out, size_t out_size) {
    if (!out || out_size == 0) return;
    out[0] = '\0';

    struct sockaddr_storage addr;
    socklen_t len = sizeof(addr);
    if (getpeername(fd, (struct sockaddr*)&addr, &len) != 0) return;

    if (addr.ss_family == AF_INET) {
        struct sockaddr_in* v4 = (struct sockaddr_in*)&addr;
        inet_ntop(AF_INET, &v4->sin_addr, out, (socklen_t)out_size);
    } else if (addr.ss_family == AF_INET6) {
        struct sockaddr_in6* v6 = (struct sockaddr_in6*)&addr;
        inet_ntop(AF_INET6, &v6->sin6_addr, out, (socklen_t)out_size);
    }
}
