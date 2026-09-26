/**
 * @file
 * The open-shop table: which character is standing at which merchant.
 *
 * An open-addressed map keyed by character id, growing when it passes half
 * full and shrinking never -- it is bounded by the number of characters with a
 * shop open at once, which is small and self-limiting, and a table that has
 * grown once has already paid for the traffic that made it grow.
 *
 * Erasure repairs the probe chain in place (backward-shift deletion) rather
 * than leaving tombstones, so a busy market street cannot degrade lookups over
 * an evening.
 */

#include "shop_session.h"
#include "log.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

/** Hold one map bucket. character_id zero marks an empty bucket. */
typedef struct {
    ShopSession session;
} SessionBucket;

static SessionBucket*  g_sessions      = NULL;
static uint32_t        g_session_mask  = 0;   /**< Capacity - 1; capacity is a power of two. */
static int             g_session_count = 0;
static pthread_mutex_t g_sessions_lock = PTHREAD_MUTEX_INITIALIZER;

#define SHOP_SESSION_MIN_CAPACITY 32u

static double now_seconds(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec / 1000000.0;
}

static inline uint32_t session_hash(uint32_t key) {
    key ^= key >> 16;
    key *= 0x7feb352dU;
    key ^= key >> 15;
    key *= 0x846ca68bU;
    key ^= key >> 16;
    return key;
}

/** Find a character's bucket, or NULL. Caller holds the lock. */
static ShopSession* session_lookup(uint32_t character_id) {
    if (!g_sessions || character_id == 0) return NULL;

    uint32_t pos = session_hash(character_id) & g_session_mask;
    for (uint32_t probe = 0; probe <= g_session_mask; probe++) {
        ShopSession* s = &g_sessions[pos].session;
        if (s->character_id == 0) return NULL;
        if (s->character_id == character_id) return s;
        pos = (pos + 1) & g_session_mask;
    }
    return NULL;
}

static int session_table_grow(void);

/**
 * Claim a bucket for a character, growing the map when it is over half full.
 *
 * Caller holds the lock.
 *
 * @return The bucket, or NULL when the map cannot grow.
 */
static ShopSession* session_claim(uint32_t character_id) {
    if (character_id == 0) return NULL;

    ShopSession* existing = session_lookup(character_id);
    if (existing) return existing;

    if (!g_sessions || (g_session_count + 1) * 2 > (int)(g_session_mask + 1)) {
        if (!session_table_grow()) return NULL;
    }

    uint32_t pos = session_hash(character_id) & g_session_mask;
    for (uint32_t probe = 0; probe <= g_session_mask; probe++) {
        ShopSession* s = &g_sessions[pos].session;
        if (s->character_id == 0) {
            memset(s, 0, sizeof(*s));
            s->character_id = character_id;
            g_session_count++;
            return s;
        }
        pos = (pos + 1) & g_session_mask;
    }
    return NULL;
}

/** Double the map and reinsert every session. Caller holds the lock. */
static int session_table_grow(void) {
    uint32_t old_capacity = g_sessions ? g_session_mask + 1 : 0;
    uint32_t new_capacity = old_capacity ? old_capacity * 2 : SHOP_SESSION_MIN_CAPACITY;

    SessionBucket* grown = calloc(new_capacity, sizeof(*grown));
    if (!grown) return 0;

    SessionBucket* old      = g_sessions;
    uint32_t       old_mask = g_session_mask;

    g_sessions      = grown;
    g_session_mask  = new_capacity - 1;
    g_session_count = 0;

    if (old) {
        for (uint32_t i = 0; i <= old_mask; i++) {
            if (old[i].session.character_id == 0) continue;
            ShopSession* slot = session_claim(old[i].session.character_id);
            if (slot) *slot = old[i].session;
        }
        free(old);
    }
    return 1;
}

/**
 * Remove a session and close the gap behind it. Caller holds the lock.
 *
 * Backward-shift deletion: every entry after the hole that probed past it is
 * moved back, so no tombstone is left and lookups stay at their natural length.
 */
static void session_erase(ShopSession* victim) {
    if (!victim || !g_sessions || victim->character_id == 0) return;

    memset(victim, 0, sizeof(*victim));
    g_session_count--;

    uint32_t start = (uint32_t)((SessionBucket*)victim - g_sessions);
    uint32_t pos   = (start + 1) & g_session_mask;
    uint32_t hole  = start;

    while (g_sessions[pos].session.character_id != 0) {
        ShopSession moved = g_sessions[pos].session;
        uint32_t    home  = session_hash(moved.character_id) & g_session_mask;

        /* Move it back only when the hole is on its probe path. */
        uint32_t to_hole = (pos - hole) & g_session_mask;
        uint32_t to_home = (pos - home) & g_session_mask;
        if (to_home >= to_hole) {
            g_sessions[hole].session = moved;
            memset(&g_sessions[pos].session, 0, sizeof(ShopSession));
            hole = pos;
        }
        pos = (pos + 1) & g_session_mask;
    }
}

int shop_session_open(uint32_t character_id, uint32_t shop_id,
                      uint32_t npc_id, float npc_x, float npc_y) {
    pthread_mutex_lock(&g_sessions_lock);

    ShopSession* session = session_claim(character_id);
    if (session) {
        session->character_id  = character_id;
        session->shop_id       = shop_id;
        session->npc_id        = npc_id;
        session->npc_x         = npc_x;
        session->npc_y         = npc_y;
        session->last_activity = now_seconds();
    }

    pthread_mutex_unlock(&g_sessions_lock);

    if (!session)
        LOG_ERROR("[SHOP] could not record an open shop for character %u", character_id);

    /* The pointer never leaves the lock; only the outcome does. */
    return session != NULL;
}

int shop_session_snapshot(uint32_t character_id, ShopSession* out) {
    pthread_mutex_lock(&g_sessions_lock);

    ShopSession* session = session_lookup(character_id);
    int open = (session != NULL);
    if (open && out) *out = *session;

    pthread_mutex_unlock(&g_sessions_lock);
    return open;
}

void shop_session_touch(uint32_t character_id) {
    pthread_mutex_lock(&g_sessions_lock);
    ShopSession* session = session_lookup(character_id);
    if (session) session->last_activity = now_seconds();
    pthread_mutex_unlock(&g_sessions_lock);
}

void shop_session_close(uint32_t character_id) {
    pthread_mutex_lock(&g_sessions_lock);
    session_erase(session_lookup(character_id));
    pthread_mutex_unlock(&g_sessions_lock);
}

void shop_session_tick(void) {
    double now = now_seconds();

    pthread_mutex_lock(&g_sessions_lock);

    /* Erasing moves later entries backwards to repair the probe chain, so the
     * sweep restarts rather than walking past a bucket that just moved. */
    int swept;
    do {
        swept = 0;
        for (uint32_t i = 0; g_sessions && i <= g_session_mask; i++) {
            ShopSession* s = &g_sessions[i].session;
            if (s->character_id == 0) continue;
            if (now - s->last_activity <= SHOP_SESSION_TIMEOUT) continue;

            LOG_DEBUG("[SHOP] shop %u timed out for character %u",
                      s->shop_id, s->character_id);
            session_erase(s);
            swept = 1;
            break;
        }
    } while (swept);

    pthread_mutex_unlock(&g_sessions_lock);
}

void shop_session_shutdown(void) {
    pthread_mutex_lock(&g_sessions_lock);
    free(g_sessions);
    g_sessions      = NULL;
    g_session_mask  = 0;
    g_session_count = 0;
    pthread_mutex_unlock(&g_sessions_lock);
}

int shop_session_count(void) {
    pthread_mutex_lock(&g_sessions_lock);
    int count = g_session_count;
    pthread_mutex_unlock(&g_sessions_lock);
    return count;
}
