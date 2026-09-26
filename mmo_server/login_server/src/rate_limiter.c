/**
 * @file
 * Enforce bounded per-address authentication and connection rates for the login server.
 */

#include "rate_limiter.h"
#include "peer_addr.h"

#include "log.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>

/** Slots the table starts with. It grows from here; it is not a ceiling. */
#define RL_TABLE_INITIAL   4096

/** Grow when this fraction of slots is live, as a percentage.
 *
 * Open addressing degrades sharply past about three-quarters full, and the
 * table used to answer that by giving up: a bounded probe run that found no
 * free slot failed closed, so a burst of addresses colliding into one chain
 * locked out unrelated legitimate addresses that happened to hash nearby.
 * Growing is the answer to a full table; refusing service is not.
 */
#define RL_GROW_AT_PERCENT 70

/** Bounds worst-case lookup cost before the table is grown instead. */
#define RL_MAX_PROBES      32

#define RL_MAX_FAILS    5       // failures inside one window before a block
#define RL_WINDOW_SECS  60      // counting window
#define RL_BLOCK_SECS   300     // block duration

/** Maximum accepted connections per address during one connection window. */
#define RL_MAX_CONNS         30
#define RL_CONN_WINDOW_SECS  60

/** Accounts one address may create per registration window.
 *
 * Registration was throttled only by the *failure* counter, and a registration
 * with a username nobody had taken always succeeded -- so it never recorded a
 * failure and never approached a limit. What bounded it was the 30 connections
 * a minute an address may open, and each of those connections could mint an
 * account at roughly 85ms of Argon2id on a server core. That is a free way to
 * fill the users table and to spend the login server's CPU.
 *
 * An hour-long window rather than a minute, because the thing being limited is
 * a person signing up, not a packet. Four accounts an hour is generous for the
 * household or office behind one NAT and useless as an amplifier.
 */
#define RL_MAX_REGISTRATIONS      4
#define RL_REG_WINDOW_SECS     3600

/** Connections one address may open per window, resolved at startup.
 *
 * A compiled-in 30 assumes one address is roughly one player, which is true
 * right up until it is not: a university, an office, or a mobile carrier puts a
 * whole population behind one NAT, and thirty connections a minute would lock
 * all of them out together. It is also what makes the service impossible to
 * load test from one machine -- the measurement stops at the limiter and never
 * reaches the server.
 *
 * So it is a number, not a constant. The default is unchanged. */
static int g_max_conns = RL_MAX_CONNS;

/** Failed attempts one address may make per window, resolved at startup.
 *
 * Same reasoning as the connection limit above, and the same shape of harm: an
 * address is not a person. Five mistyped passwords from one office block the
 * whole office for five minutes. The default is unchanged. */
static int g_max_fails = RL_MAX_FAILS;

/** Accounts one address may create per window, resolved at startup.
 *
 * Same reasoning as the two limits above: a shared address is not one person,
 * and a deployment that knows its own topology can say so. */
static int g_max_registrations = RL_MAX_REGISTRATIONS;

/** IPv6 prefix length one bucket covers, resolved at startup. */
static int g_ipv6_prefix = RL_IPV6_PREFIX_DEFAULT;

/** Track failure, connection, and blocking windows for one peer bucket. */
typedef struct {
    char key[RL_KEY_MAXLEN];  // empty string = slot never used
    int  fail_count;
    long window_start;      // when the current failure window began
    int  conn_count;
    long conn_window_start; // when the current connection window began
    int  reg_count;
    long reg_window_start;  // when the current registration window began
    long block_until;       // 0 = not blocked
    long last_seen;         // last time this entry was touched
} RateLimitEntry;

/** The table. Grows with the population it is asked to police. */
static RateLimitEntry* g_entries  = NULL;
static size_t          g_capacity = 0;
static size_t          g_live     = 0;   /**< Occupied slots, dead ones included. */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

// Monotonic seconds. Wall-clock steps (NTP, manual clock changes) must not be
// able to shorten a block or reset a counting window.
static long rl_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec;
}

/* --- Bucketing ----------------------------------------------------------- */

/**
 * Reduce a textual peer address to the key the limiter counts against.
 *
 * @return 1 when a key was written, or 0 when the address is unusable.
 */
int rate_limiter_bucket_key(const char* ip, char* out, size_t out_size) {
    if (!ip || !*ip || !out || out_size == 0) return 0;

    struct in6_addr v6;
    if (inet_pton(AF_INET6, ip, &v6) == 1) {
        /* An IPv4-mapped address is an IPv4 host that happens to have arrived
         * on an AF_INET6 socket. Bucketing it as IPv6 would give the same host
         * two independent budgets depending on how it connected. */
        static const unsigned char v4_mapped_prefix[12] = {
            0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff
        };
        if (memcmp(v6.s6_addr, v4_mapped_prefix, sizeof(v4_mapped_prefix)) == 0) {
            struct in_addr v4;
            memcpy(&v4.s_addr, v6.s6_addr + 12, 4);
            char text[INET_ADDRSTRLEN];
            if (!inet_ntop(AF_INET, &v4, text, sizeof(text))) return 0;
            return snprintf(out, out_size, "%s", text) > 0;
        }

        int prefix = g_ipv6_prefix;
        if (prefix < 0)   prefix = 0;
        if (prefix > 128) prefix = 128;

        /* Zero every bit past the prefix, so an attacker holding a routed
         * allocation gets one budget rather than one per address in it. */
        for (int bit = prefix; bit < 128; bit++)
            v6.s6_addr[bit / 8] &= (unsigned char)~(0x80u >> (bit % 8));

        char text[INET6_ADDRSTRLEN];
        if (!inet_ntop(AF_INET6, &v6, text, sizeof(text))) return 0;
        return snprintf(out, out_size, "%s/%d", text, prefix) > 0;
    }

    struct in_addr v4;
    if (inet_pton(AF_INET, ip, &v4) == 1)
        return snprintf(out, out_size, "%s", ip) > 0;

    /* Not an address this limiter can reason about. Callers fail closed. */
    return 0;
}

/* --- The table ----------------------------------------------------------- */

/** Compute the FNV-1a hash used to place a bucket key. */
static uint32_t hash_key(const char* key) {
    uint32_t h = 2166136261u;                   // FNV-1a
    for (const unsigned char* p = (const unsigned char*)key; *p; p++) {
        h ^= (uint32_t)*p;
        h *= 16777619u;
    }
    return h;
}

/** Report whether an occupied entry may be reclaimed. */
static int entry_is_dead(const RateLimitEntry* e, long now) {
    if (e->key[0] == '\0')     return 0;   // never used, not "dead"
    if (e->block_until > now)  return 0;   // actively blocked, must keep
    return (now - e->last_seen) > RL_WINDOW_SECS;
}

/** Allocate the table, or replace it with a larger one. Caller holds g_lock.
 *
 * Dead entries are dropped rather than carried across, so a rehash is also the
 * sweep this table never had.
 *
 * @return 1 on success, or 0 when the allocation fails (the old table stands).
 */
static int table_resize(size_t new_capacity, long now) {
    RateLimitEntry* fresh = calloc(new_capacity, sizeof(*fresh));
    if (!fresh) {
        LOG_ERROR("[RATE_LIMIT] cannot grow the table to %zu slots", new_capacity);
        return 0;
    }

    size_t mask = new_capacity - 1;
    size_t moved = 0;

    for (size_t i = 0; i < g_capacity; i++) {
        RateLimitEntry* old = &g_entries[i];
        if (old->key[0] == '\0') continue;
        if (entry_is_dead(old, now)) continue;

        uint32_t h = hash_key(old->key);
        for (size_t probe = 0; probe < new_capacity; probe++) {
            RateLimitEntry* slot = &fresh[(h + probe) & mask];
            if (slot->key[0] == '\0') { *slot = *old; moved++; break; }
        }
    }

    free(g_entries);
    g_entries  = fresh;
    g_capacity = new_capacity;
    g_live     = moved;
    return 1;
}

/** Ensure the table exists. Caller holds g_lock. @return 1 when usable. */
static int table_ready(long now) {
    if (g_capacity > 0) return 1;
    return table_resize(RL_TABLE_INITIAL, now);
}

/**
 * Find a live entry without creating one.
 *
 * @return      The matching entry, or NULL when the bucket is absent or stale.
 */
static RateLimitEntry* find(const char* key, long now) {
    if (g_capacity == 0) return NULL;

    size_t mask = g_capacity - 1;
    uint32_t h = hash_key(key);
    for (uint32_t i = 0; i < RL_MAX_PROBES; i++) {
        RateLimitEntry* e = &g_entries[(h + i) & mask];
        if (e->key[0] == '\0') return NULL;             // chain ends here
        if (strcmp(e->key, key) == 0)
            return entry_is_dead(e, now) ? NULL : e;    // stale entry reads as absent
    }
    return NULL;
}

/** Claim a slot for a key in the current table, without growing it.
 *
 * @return The entry, or NULL when the bounded probe range is saturated.
 */
static RateLimitEntry* find_or_create_in_table(const char* key, long now) {
    if (g_capacity == 0) return NULL;

    size_t mask = g_capacity - 1;
    uint32_t h = hash_key(key);
    RateLimitEntry* reusable = NULL;

    for (uint32_t i = 0; i < RL_MAX_PROBES; i++) {
        RateLimitEntry* e = &g_entries[(h + i) & mask];

        if (e->key[0] == '\0') {
            // Virgin slot ends the chain. Reuse an earlier dead slot if we
            // passed one; placing the entry earlier keeps future probes short.
            RateLimitEntry* target = reusable ? reusable : e;
            int fresh_slot = (target == e);
            memset(target, 0, sizeof(*target));
            snprintf(target->key, sizeof(target->key), "%s", key);
            target->window_start      = now;
            target->conn_window_start = now;
            target->last_seen         = now;
            if (fresh_slot) g_live++;
            return target;
        }

        if (strcmp(e->key, key) == 0) {
            if (entry_is_dead(e, now)) {
                // Same bucket, but its history has lapsed. Start it fresh
                // rather than resurrecting a stale fail_count.
                memset(e, 0, sizeof(*e));
                snprintf(e->key, sizeof(e->key), "%s", key);
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
        snprintf(reusable->key, sizeof(reusable->key), "%s", key);
        reusable->window_start      = now;
        reusable->conn_window_start = now;
        reusable->last_seen         = now;
        return reusable;
    }

    return NULL;   // this table is too crowded for this chain
}

/**
 * Find a bucket entry, growing the table rather than refusing service.
 *
 * @return      The entry, or NULL only when memory cannot be had.
 */
static RateLimitEntry* find_or_create(const char* key, long now) {
    if (!table_ready(now)) return NULL;

    /* Grow before the load factor gets high enough to make probe runs long,
     * not after a probe run has already failed. */
    if (g_live * 100 >= g_capacity * RL_GROW_AT_PERCENT)
        table_resize(g_capacity * 2, now);

    RateLimitEntry* e = find_or_create_in_table(key, now);
    if (e) return e;

    /* One crowded chain, in a table that is not otherwise full. Rehashing
     * redistributes it; doubling guarantees room. */
    if (!table_resize(g_capacity * 2, now)) return NULL;
    return find_or_create_in_table(key, now);
}

/** Entries the limiter table currently holds. */
size_t rate_limiter_tracked(void) {
    pthread_mutex_lock(&g_lock);
    size_t live = g_live;
    pthread_mutex_unlock(&g_lock);
    return live;
}

/** Slots the limiter table has grown to. */
size_t rate_limiter_capacity(void) {
    pthread_mutex_lock(&g_lock);
    size_t cap = g_capacity;
    pthread_mutex_unlock(&g_lock);
    return cap;
}

/** Clear all rate-limit entries and read the configurable limits. */
void rate_limiter_init(void) {
    pthread_mutex_lock(&g_lock);
    free(g_entries);
    g_entries  = NULL;
    g_capacity = 0;
    g_live     = 0;

    /* Reset to the compiled defaults before reading the environment, so this
     * function establishes the configuration rather than accumulating it. A
     * setting removed between two calls must actually go away. */
    g_max_fails         = RL_MAX_FAILS;
    g_max_conns         = RL_MAX_CONNS;
    g_max_registrations = RL_MAX_REGISTRATIONS;
    g_ipv6_prefix       = RL_IPV6_PREFIX_DEFAULT;

    const char* failures = getenv("MMO_LOGIN_MAX_FAILS_PER_MIN");
    if (failures && *failures) {
        int value = atoi(failures);
        if (value > 0) {
            g_max_fails = value;
            LOG_WARN("[RATE_LIMIT] per-address failure limit set to %d/%ds "
                     "by MMO_LOGIN_MAX_FAILS_PER_MIN (default %d)",
                     g_max_fails, RL_WINDOW_SECS, RL_MAX_FAILS);
        } else {
            LOG_ERROR("[RATE_LIMIT] MMO_LOGIN_MAX_FAILS_PER_MIN='%s' is not a "
                      "positive number; keeping the default of %d",
                      failures, RL_MAX_FAILS);
        }
    }

    const char* configured = getenv("MMO_LOGIN_MAX_CONNS_PER_MIN");
    if (configured && *configured) {
        int value = atoi(configured);
        if (value > 0) {
            g_max_conns = value;
            LOG_WARN("[RATE_LIMIT] per-address connection limit set to %d/%ds "
                     "by MMO_LOGIN_MAX_CONNS_PER_MIN (default %d)",
                     g_max_conns, RL_CONN_WINDOW_SECS, RL_MAX_CONNS);
        } else {
            LOG_ERROR("[RATE_LIMIT] MMO_LOGIN_MAX_CONNS_PER_MIN='%s' is not a "
                      "positive number; keeping the default of %d",
                      configured, RL_MAX_CONNS);
        }
    }
    const char* registrations = getenv("MMO_LOGIN_MAX_REGISTRATIONS_PER_HOUR");
    if (registrations && *registrations) {
        int value = atoi(registrations);
        if (value > 0) {
            g_max_registrations = value;
            LOG_WARN("[RATE_LIMIT] per-address registration limit set to %d/%ds "
                     "by MMO_LOGIN_MAX_REGISTRATIONS_PER_HOUR (default %d)",
                     g_max_registrations, RL_REG_WINDOW_SECS, RL_MAX_REGISTRATIONS);
        } else {
            LOG_ERROR("[RATE_LIMIT] MMO_LOGIN_MAX_REGISTRATIONS_PER_HOUR='%s' is not a "
                      "positive number; keeping the default of %d",
                      registrations, RL_MAX_REGISTRATIONS);
        }
    }

    const char* prefix = getenv("MMO_LOGIN_IPV6_PREFIX");
    if (prefix && *prefix) {
        int value = atoi(prefix);
        if (value >= 0 && value <= 128) {
            g_ipv6_prefix = value;
            LOG_WARN("[RATE_LIMIT] IPv6 addresses counted per /%d "
                     "by MMO_LOGIN_IPV6_PREFIX (default /%d)",
                     g_ipv6_prefix, RL_IPV6_PREFIX_DEFAULT);
        } else {
            LOG_ERROR("[RATE_LIMIT] MMO_LOGIN_IPV6_PREFIX='%s' is not in [0, 128]; "
                      "keeping the default of /%d", prefix, RL_IPV6_PREFIX_DEFAULT);
        }
    }

    table_ready(rl_now());
    pthread_mutex_unlock(&g_lock);
}

/**
 * Test whether an address is blocked or cannot be identified.
 *
 * @return      Nonzero when the connection must be rejected, otherwise zero.
 */
int rate_limiter_check(const char* ip) {
    // No usable address means no way to police this connection.
    char key[RL_KEY_MAXLEN];
    if (!rate_limiter_bucket_key(ip, key, sizeof(key))) return 1;

    pthread_mutex_lock(&g_lock);
    long now = rl_now();
    RateLimitEntry* e = find(key, now);
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
    char key[RL_KEY_MAXLEN];
    if (!rate_limiter_bucket_key(ip, key, sizeof(key))) return 1;  // untrackable: fail closed

    pthread_mutex_lock(&g_lock);
    long now = rl_now();

    RateLimitEntry* e = find_or_create(key, now);
    if (!e) {
        pthread_mutex_unlock(&g_lock);
        LOG_WARN_RL(5, 60,
                    "[RATE_LIMIT] out of memory, cannot track %s — dropping connection",
                    key);
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
    if (e->fail_count >= g_max_fails) {
        e->block_until = now + RL_BLOCK_SECS;
        drop = 1;
    }
    int fails = e->fail_count;
    pthread_mutex_unlock(&g_lock);

    if (drop)
        LOG_WARN("[RATE_LIMIT] %s blocked for %ds (%d failures in %ds)",
                 key, RL_BLOCK_SECS, fails, RL_WINDOW_SECS);

    return drop;
}

/**
 * Record an accepted connection and apply the connection-churn threshold.
 *
 * @return      Nonzero when the connection must be refused, otherwise zero.
 */
int rate_limiter_record_connection(const char* ip) {
    char key[RL_KEY_MAXLEN];
    if (!rate_limiter_bucket_key(ip, key, sizeof(key))) return 1;  // untrackable: fail closed

    pthread_mutex_lock(&g_lock);
    long now = rl_now();

    RateLimitEntry* e = find_or_create(key, now);
    if (!e) {
        pthread_mutex_unlock(&g_lock);
        LOG_WARN_RL(5, 60,
                    "[RATE_LIMIT] out of memory, cannot track %s — refusing connection",
                    key);
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
    if (e->conn_count > g_max_conns) {
        // Churning connections is as hostile as churning passwords, so it
        // earns the same block rather than a per-connection brush-off.
        e->block_until = now + RL_BLOCK_SECS;
        refuse = 1;
    }
    int conns = e->conn_count;
    pthread_mutex_unlock(&g_lock);

    if (refuse)
        LOG_WARN("[RATE_LIMIT] %s blocked for %ds (%d connections in %ds)",
                 key, RL_BLOCK_SECS, conns, RL_CONN_WINDOW_SECS);

    return refuse;
}

/**
 * Spend one unit of an address's registration budget.
 *
 * Charged on the *attempt*, before the account is created, not on success:
 * charging only successes would let an attacker probe usernames for free, and
 * charging only failures is exactly the hole this closes. A blocked address is
 * refused without spending anything.
 *
 * @return Nonzero when the registration must be refused, otherwise zero.
 */
int rate_limiter_record_registration(const char* ip) {
    char key[RL_KEY_MAXLEN];
    if (!rate_limiter_bucket_key(ip, key, sizeof(key))) return 1;  // untrackable: fail closed

    pthread_mutex_lock(&g_lock);
    long now = rl_now();

    RateLimitEntry* e = find_or_create(key, now);
    if (!e) {
        pthread_mutex_unlock(&g_lock);
        LOG_WARN_RL(5, 60,
                    "[RATE_LIMIT] out of memory, cannot track %s — refusing registration",
                    key);
        return 1;   // fail closed
    }

    if (e->block_until > now) {
        pthread_mutex_unlock(&g_lock);
        return 1;
    }

    if (e->reg_window_start == 0 || now - e->reg_window_start > RL_REG_WINDOW_SECS) {
        e->reg_count        = 0;
        e->reg_window_start = now;
    }

    e->reg_count++;

    /* Over budget refuses the registration but does not block the address
     * outright: the same household still has to be able to log in to the
     * accounts it already has. */
    int refuse = (e->reg_count > g_max_registrations);
    int count  = e->reg_count;
    pthread_mutex_unlock(&g_lock);

    if (refuse)
        LOG_WARN("[RATE_LIMIT] %s refused registration (%d attempts in %ds, limit %d)",
                 key, count, RL_REG_WINDOW_SECS, g_max_registrations);

    return refuse;
}

/** Clear expired failure history after successful authentication. */
void rate_limiter_note_success(const char* ip) {
    char key[RL_KEY_MAXLEN];
    if (!rate_limiter_bucket_key(ip, key, sizeof(key))) return;

    pthread_mutex_lock(&g_lock);
    long now = rl_now();
    RateLimitEntry* e = find(key, now);
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
 * Delegates to the shared resolver so the realm allowlist, session binding, and
 * this limiter all read the same address for the same descriptor.
 *
 * Failure is reported by leaving the output as an empty string.
 */
void rate_limiter_peer_ip(int fd, char* out, size_t out_size) {
    peer_addr_text(fd, out, out_size);
}
