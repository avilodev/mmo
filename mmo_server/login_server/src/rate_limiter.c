// ============================================================================
// rate_limiter.c — Per-IP brute-force protection for the login server
//
// Tracks failed auth attempts per IP address. After RL_MAX_FAILS failures
// within RL_WINDOW_SECS seconds, the IP is blocked for RL_BLOCK_SECS.
// The table is a fixed-size linear array — suitable for the load this server
// expects (<< 1000 concurrent unique IPs during an attack window).
// ============================================================================

#include "rate_limiter.h"

#include <string.h>
#include <time.h>
#include <stdio.h>
#include <pthread.h>

#define RL_MAX_ENTRIES  256     // max distinct IPs tracked simultaneously
#define RL_MAX_FAILS    5       // failures before block
#define RL_WINDOW_SECS  60      // counting window (seconds)
#define RL_BLOCK_SECS   300     // block duration (seconds)

typedef struct {
    char    ip[16];             // dotted-decimal, e.g. "192.168.1.1"
    int     fail_count;
    time_t  window_start;       // when the current window began
    time_t  block_until;        // 0 = not blocked
} RateLimitEntry;

static RateLimitEntry   g_entries[RL_MAX_ENTRIES];
static int              g_count = 0;
static pthread_mutex_t  g_lock  = PTHREAD_MUTEX_INITIALIZER;

void rate_limiter_init(void) {
    memset(g_entries, 0, sizeof(g_entries));
    g_count = 0;
}

// Find an existing entry or claim a new slot. Returns NULL if table is full.
static RateLimitEntry* find_or_create(const char* ip) {
    for (int i = 0; i < g_count; i++) {
        if (strcmp(g_entries[i].ip, ip) == 0)
            return &g_entries[i];
    }
    if (g_count >= RL_MAX_ENTRIES) return NULL;
    RateLimitEntry* e = &g_entries[g_count++];
    memset(e, 0, sizeof(*e));
    strncpy(e->ip, ip, sizeof(e->ip) - 1);
    return e;
}

int rate_limiter_check(const char* ip) {
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < g_count; i++) {
        if (strcmp(g_entries[i].ip, ip) == 0) {
            int blocked = (g_entries[i].block_until > time(NULL));
            pthread_mutex_unlock(&g_lock);
            return blocked;
        }
    }
    pthread_mutex_unlock(&g_lock);
    return 0;
}

void rate_limiter_record_failure(const char* ip) {
    pthread_mutex_lock(&g_lock);

    RateLimitEntry* e = find_or_create(ip);
    if (!e) {
        // Table full — just log and move on; the connection will still be
        // processed (best-effort protection).
        printf("[RATE_LIMIT] Warning: table full, cannot track %s\n", ip);
        pthread_mutex_unlock(&g_lock);
        return;
    }

    time_t now = time(NULL);

    // Reset window if it has expired
    if (now - e->window_start > RL_WINDOW_SECS) {
        e->fail_count   = 0;
        e->window_start = now;
    }

    e->fail_count++;

    if (e->fail_count >= RL_MAX_FAILS) {
        e->block_until = now + RL_BLOCK_SECS;
        printf("[RATE_LIMIT] IP %s blocked for %d seconds (%d failures in %ds window)\n",
               ip, RL_BLOCK_SECS, e->fail_count, RL_WINDOW_SECS);
    }

    pthread_mutex_unlock(&g_lock);
}
