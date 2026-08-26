/**
 * @file
 * The character-name cache and the batched query that fills it.
 */

#include "network/name_cache.h"
#include "net_internal.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

/** Seconds before an unanswered identifier may be asked about again.
 *
 * An identifier that never resolves -- someone who logged out between being
 * seen and being asked about -- would otherwise be re-queued every frame, and
 * the batch would go out sixty times a second forever. */
#define NAME_CACHE_RETRY_SECONDS 30.0

/** One cached identifier. `name[0] == 0` means asked but not yet answered. */
typedef struct {
    uint32_t character_id;   /**< 0 marks a free slot. */
    char     name[32];
    double   asked_at;       /**< Monotonic seconds; 0 when never asked. */
    double   last_used;      /**< For recycling the least recently wanted. */
} NameEntry;

static NameEntry g_entries[NAME_CACHE_CAPACITY];

/** Identifiers queued for the next request. */
static uint32_t g_pending[MAX_NAME_QUERY];
static int      g_pending_count;

static double now_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

void name_cache_reset(void) {
    memset(g_entries, 0, sizeof(g_entries));
    g_pending_count = 0;
}

/** Find an identifier's entry, or NULL. */
static NameEntry* find(uint32_t character_id) {
    for (int i = 0; i < NAME_CACHE_CAPACITY; i++) {
        if (g_entries[i].character_id == character_id) return &g_entries[i];
    }
    return NULL;
}

/**
 * Claim a slot for an identifier: a free one, or the least recently wanted.
 *
 * Recycling rather than refusing, because the identifiers that matter are the
 * ones currently on screen and those are the ones being looked up. A full
 * table full of people who left is exactly what should be overwritten.
 */
static NameEntry* claim(uint32_t character_id) {
    NameEntry* oldest = &g_entries[0];

    for (int i = 0; i < NAME_CACHE_CAPACITY; i++) {
        if (g_entries[i].character_id == 0) {
            memset(&g_entries[i], 0, sizeof(g_entries[i]));
            g_entries[i].character_id = character_id;
            return &g_entries[i];
        }
        if (g_entries[i].last_used < oldest->last_used) oldest = &g_entries[i];
    }

    memset(oldest, 0, sizeof(*oldest));
    oldest->character_id = character_id;
    return oldest;
}

/** Add an identifier to the outgoing batch if it is not already in it. */
static void queue_request(uint32_t character_id) {
    if (g_pending_count >= MAX_NAME_QUERY) return;
    for (int i = 0; i < g_pending_count; i++) {
        if (g_pending[i] == character_id) return;
    }
    g_pending[g_pending_count++] = character_id;
}

const char* name_cache_lookup(uint32_t character_id) {
    /* Returned when the name has not arrived. Deliberately not the identifier:
     * a number above a character's head reads as a name, and players believed
     * "Player_1042" was one. An ellipsis reads as "loading". */
    static const char* k_pending_label = "…";

    /* Per-call storage for the returned string, so a caller can hold it across
     * one draw without the entry being recycled underneath it. */
    static char returned[32];

    if (character_id == 0) return k_pending_label;

    double now = now_seconds();
    NameEntry* entry = find(character_id);

    if (!entry) {
        entry = claim(character_id);
        entry->asked_at = now;
        queue_request(character_id);
    } else if (entry->name[0] == '\0' &&
               now - entry->asked_at >= NAME_CACHE_RETRY_SECONDS) {
        /* Still unanswered after the retry window. Ask once more -- the
         * character may have logged back in -- but not before. */
        entry->asked_at = now;
        queue_request(character_id);
    }

    entry->last_used = now;

    if (entry->name[0] == '\0') return k_pending_label;

    snprintf(returned, sizeof(returned), "%s", entry->name);
    return returned;
}

void name_cache_store(uint32_t character_id, const char* name) {
    if (character_id == 0 || !name || name[0] == '\0') return;

    NameEntry* entry = find(character_id);
    if (!entry) entry = claim(character_id);

    snprintf(entry->name, sizeof(entry->name), "%s", name);
    entry->last_used = now_seconds();
}

void name_cache_flush_requests(void) {
    if (g_pending_count == 0) return;
    if (!g_net.connected) { g_pending_count = 0; return; }

    NameQueryRequestPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.header.type = PACKET_NAME_QUERY_REQUEST;
    pkt.header.player_id = htonl(g_net.character_id);

    int count = g_pending_count;
    if (count > MAX_NAME_QUERY) count = MAX_NAME_QUERY;
    pkt.count = (uint8_t)count;
    for (int i = 0; i < count; i++) pkt.character_ids[i] = htonl(g_pending[i]);

    /* Only the identifiers actually being asked about. The array is sized for
     * the worst case; sending all of it would spend 128 bytes to ask one
     * question. */
    size_t send_size = offsetof(NameQueryRequestPacket, character_ids) +
                       (size_t)count * sizeof(uint32_t);
    pkt.header.payload_size = (uint16_t)htons((uint16_t)(send_size - sizeof(PacketHeader)));

    net_send((const char*)&pkt, (int)send_size);
    g_pending_count = 0;
}

int name_cache_known_count(void) {
    int count = 0;
    for (int i = 0; i < NAME_CACHE_CAPACITY; i++) {
        if (g_entries[i].character_id != 0 && g_entries[i].name[0] != '\0') count++;
    }
    return count;
}

int name_cache_pending_count(void) { return g_pending_count; }
