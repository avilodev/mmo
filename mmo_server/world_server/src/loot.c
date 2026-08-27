/**
 * @file
 * Load NPC loot tables and manage world-server ground-item lifetimes and pickup.
 *
 * Two things used to be true of this file that are not any more.
 *
 * The pools were fixed arrays -- 256 ground items, 64 loot tables, 8 drops per
 * table -- and every one of those was a silent content limit: the 65th loot
 * table simply did not load, and the ninth drop authored for an NPC was
 * reported and thrown away. They grow now, from the file and from play.
 *
 * And pickup destroyed the item before it knew the player could hold it. It
 * reserves now, inserts, and only then commits.
 */

#include "loot.h"
#include "log.h"
#include "types.h"
#include "json_util.h"
#include "player_data.h"
#include "interest.h"
#include "items_database.h"
#include "quest_system.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <pthread.h>
#include <time.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include "utils.h"

extern ActivePlayer active_players[];

/** Loot tables, grown from the JSON registry. */
static LootTable* g_loot_tables      = NULL;
static int        g_loot_table_count = 0;
static int        g_loot_table_cap   = 0;

/** The world floor. Grows when every slot is occupied; slots are reused. */
static GroundItem*     g_ground_items     = NULL;
static size_t          g_ground_capacity  = 0;
static pthread_mutex_t g_ground_items_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t        g_next_ground_item_id = 1;

/** Slots the pool starts with. It grows from here; it is not a ceiling. */
#define GROUND_POOL_INITIAL 256

/** Log once when the floor grows past this, so runaway growth is visible. */
#define GROUND_POOL_NOTABLE 4096

static double get_time(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static const LootTable* find_loot_table(uint16_t npc_type_id) {
    for (int i = 0; i < g_loot_table_count; i++) {
        if (g_loot_tables[i].npc_type_id == npc_type_id) {
            return &g_loot_tables[i];
        }
    }
    return NULL;
}

/* --- The ground-item pool ------------------------------------------------ */

/** Find a free slot, growing the pool when every slot is taken.
 *
 * Caller holds g_ground_items_lock.
 *
 * @return The slot index, or -1 when the pool cannot grow.
 */
static int ground_claim_slot_locked(void) {
    for (size_t i = 0; i < g_ground_capacity; i++)
        if (!g_ground_items[i].active) return (int)i;

    size_t grown = g_ground_capacity ? g_ground_capacity * 2 : GROUND_POOL_INITIAL;
    GroundItem* bigger = realloc(g_ground_items, grown * sizeof(*bigger));
    if (!bigger) {
        LOG_ERROR("[LOOT] Cannot grow the ground-item pool past %zu slots",
                  g_ground_capacity);
        return -1;
    }
    memset(bigger + g_ground_capacity, 0,
           (grown - g_ground_capacity) * sizeof(*bigger));

    size_t previous = g_ground_capacity;
    g_ground_items    = bigger;
    g_ground_capacity = grown;

    if (grown >= GROUND_POOL_NOTABLE && previous < GROUND_POOL_NOTABLE) {
        LOG_WARN("[LOOT] Ground-item pool grew to %zu slots. Items despawn after "
                 "%.0fs, so a floor this large means drops are outpacing pickups.",
                 grown, LOOT_DESPAWN_TIME);
    }
    return (int)previous;
}

/** Locate an active ground item by identifier. Caller holds the lock.
 *
 * @return The item, or NULL when no active item carries that identifier.
 */
static GroundItem* ground_find_locked(uint32_t ground_item_id) {
    for (size_t i = 0; i < g_ground_capacity; i++) {
        if (g_ground_items[i].active && g_ground_items[i].id == ground_item_id)
            return &g_ground_items[i];
    }
    return NULL;
}

/* --- Sending ------------------------------------------------------------- */

/** Send one packet to every online player within `radius` of a world point.
 *
 * The descriptor list is sized from the player table rather than from a
 * literal. Both broadcast paths here used a `fds[64]`, which is not a view
 * radius but a lost notification: past the 64th player near a drop, the rest
 * were never told it existed.
 */
static void send_to_nearby(float x, float y, float radius,
                           void* packet, size_t size) {
    int* fds = malloc(sizeof(int) * MAX_PLAYERS);
    if (!fds) return;

    int n = interest_collect_fds(x, y, radius, fds, MAX_PLAYERS);
    for (int i = 0; i < n; i++) server_send(fds[i], packet, size);

    free(fds);
}

/** Interest radius for a ground item appearing or expiring, in world pixels.
 *
 * Matches the player view radius: loot you cannot see vanishing is not news.
 */
#define LOOT_VIEW_RADIUS INTEREST_EVENT_RADIUS

/** Tell nearby players a ground item is gone. */
static void broadcast_despawn(uint32_t ground_item_id, float x, float y) {
    LootDespawnPacket pkt = {0};
    pkt.header.type         = PACKET_LOOT_DESPAWN;
    pkt.header.player_id    = 0;
    pkt.header.payload_size = htons(sizeof(LootDespawnPacket) - sizeof(PacketHeader));
    pkt.ground_item_id      = htonl(ground_item_id);

    send_to_nearby(x, y, LOOT_VIEW_RADIUS, &pkt, sizeof(pkt));
}

/** Tell nearby players a ground item has appeared. */
static void broadcast_drop(uint32_t ground_id, uint32_t item_id, uint8_t quantity,
                           float x, float y) {
    LootDropPacket pkt = {0};
    pkt.header.type         = PACKET_LOOT_DROP;
    pkt.header.player_id    = 0;
    pkt.header.payload_size = htons(sizeof(LootDropPacket) - sizeof(PacketHeader));
    pkt.ground_item_id      = htonl(ground_id);
    pkt.item_id             = htonl(item_id);
    pkt.quantity            = quantity;
    pkt.pos_x               = x;
    pkt.pos_y               = y;

    send_to_nearby(x, y, LOOT_VIEW_RADIUS, &pkt, sizeof(pkt));
}

/* --- JSON --------------------------------------------------------------- */

/**
 * Read an entire file into a terminated buffer.
 *
 * The caller must free the returned buffer.
 *
 * @return An allocated buffer, or NULL when opening or allocation fails.
 */
static char* read_file(const char* filepath) {
    FILE* f = fopen(filepath, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 0) { fclose(f); return NULL; }
    char* buf = malloc((size_t)size + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)size, f);
    buf[got] = '\0';
    fclose(f);
    return buf;
}

static const char* skip_ws(const char* p) {
    while (*p && isspace((unsigned char)*p)) p++;
    return p;
}

static const char* find_key(const char* json, const char* key) {
    char search[128];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char* pos = strstr(json, search);
    if (!pos) return NULL;
    pos = strchr(pos, ':');
    if (!pos) return NULL;
    pos++;
    return skip_ws(pos);
}

/**
 * Find the delimiter that balances a nested JSON span.
 *
 * @return The matching delimiter, or NULL for an unterminated span.
 */
static const char* find_matching(const char* start, char open, char close) {
    int depth = 0;
    const char* p = start;
    while (*p) {
        if (*p == open) depth++;
        if (*p == close) { depth--; if (depth == 0) return p; }
        p++;
    }
    return NULL;
}

/** Make room for one more loot table. @return 1 on success, 0 on allocation failure. */
static int loot_tables_reserve_one(void) {
    if (g_loot_table_count < g_loot_table_cap) return 1;

    int grown = g_loot_table_cap ? g_loot_table_cap * 2 : 16;
    LootTable* bigger = realloc(g_loot_tables, (size_t)grown * sizeof(*bigger));
    if (!bigger) return 0;
    memset(bigger + g_loot_table_cap, 0,
           (size_t)(grown - g_loot_table_cap) * sizeof(*bigger));
    g_loot_tables    = bigger;
    g_loot_table_cap = grown;
    return 1;
}

/** Make room for one more drop in a table. @return 1 on success, 0 on failure. */
static int loot_entries_reserve_one(LootTable* lt) {
    if (lt->entry_count < lt->entry_capacity) return 1;

    int grown = lt->entry_capacity ? lt->entry_capacity * 2 : 8;
    LootEntry* bigger = realloc(lt->entries, (size_t)grown * sizeof(*bigger));
    if (!bigger) return 0;
    memset(bigger + lt->entry_capacity, 0,
           (size_t)(grown - lt->entry_capacity) * sizeof(*bigger));
    lt->entries        = bigger;
    lt->entry_capacity = grown;
    return 1;
}

/** Parse the drops array of one loot table object.
 *
 * @return 1 on success, or 0 on allocation failure.
 */
static int parse_drops(LootTable* lt, const char* obj) {
    const char* drops_start = strstr(obj, "\"drops\"");
    if (!drops_start) return 1;

    const char* drops_arr = strchr(drops_start, '[');
    if (!drops_arr) return 1;
    if (!find_matching(drops_arr, '[', ']')) return 1;

    const char* dp = drops_arr + 1;
    while (*dp) {
        dp = skip_ws(dp);
        if (*dp == ']') break;
        if (*dp != '{') { dp++; continue; }

        const char* de = find_matching(dp, '{', '}');
        if (!de) break;

        size_t de_len = (size_t)(de - dp + 1);
        char* drop_obj = malloc(de_len + 1);
        if (!drop_obj) {
            LOG_ERROR("[LOOT] malloc failed while parsing drop entry");
            return 0;
        }
        memcpy(drop_obj, dp, de_len);
        drop_obj[de_len] = '\0';

        if (!loot_entries_reserve_one(lt)) {
            LOG_ERROR("[LOOT] cannot grow the drop list for npc_type_id=%u",
                      lt->npc_type_id);
            free(drop_obj);
            return 0;
        }

        LootEntry* entry = &lt->entries[lt->entry_count];
        memset(entry, 0, sizeof(*entry));

        const char* v;
        v = find_key(drop_obj, "item_id");
        if (v) entry->item_id = (uint32_t)atoi(v);

        v = find_key(drop_obj, "chance");
        if (v) entry->drop_chance = (float)atof(v);

        v = find_key(drop_obj, "min_qty");
        entry->min_qty = v ? (uint8_t)atoi(v) : 1;

        v = find_key(drop_obj, "max_qty");
        entry->max_qty = v ? (uint8_t)atoi(v) : 1;

        lt->entry_count++;
        free(drop_obj);

        dp = de + 1;
        while (*dp && *dp != ',' && *dp != ']') dp++;
        if (*dp == ',') dp++;
    }
    return 1;
}

/**
 * Parse loot-table entries from the item registry.
 *
 * @param source  Path the JSON came from, named in diagnostics.
 * @return 1 when parsing completes or no section exists, or 0 on malformed
 *         input or allocation failure.
 */
static int parse_loot_tables(const char* json, const char* source) {
    const char* tables_start = strstr(json, "\"loot_tables\"");
    if (!tables_start) {
        LOG_DEBUG("[LOOT] No loot_tables section found in JSON");
        return 1; // Not an error — just no loot tables defined
    }

    const char* arr_start = strchr(tables_start, '[');
    if (!arr_start) return 0;
    const char* arr_end = find_matching(arr_start, '[', ']');
    if (!arr_end) return 0;

    // Extract the array content
    size_t arr_len = (size_t)(arr_end - arr_start + 1);
    char* arr_json = malloc(arr_len + 1);
    if (!arr_json) {
        LOG_ERROR("[LOOT] malloc failed while parsing loot_tables");
        return 0;
    }
    memcpy(arr_json, arr_start, arr_len);
    arr_json[arr_len] = '\0';

    // Parse each loot table object
    const char* pos = arr_json + 1; // skip '['
    while (*pos) {
        pos = skip_ws(pos);
        if (*pos == ']') break;
        if (*pos != '{') { pos++; continue; }

        const char* obj_end = find_matching(pos, '{', '}');
        if (!obj_end) break;

        size_t obj_len = (size_t)(obj_end - pos + 1);
        char* obj = malloc(obj_len + 1);
        if (!obj) {
            LOG_ERROR("[LOOT] malloc failed while parsing loot table object");
            free(arr_json);
            return 0;
        }
        memcpy(obj, pos, obj_len);
        obj[obj_len] = '\0';

        if (!loot_tables_reserve_one()) {
            LOG_ERROR("[LOOT] %s: cannot grow the loot table list past %d",
                      source, g_loot_table_cap);
            free(obj);
            free(arr_json);
            return 0;
        }

        LootTable* lt = &g_loot_tables[g_loot_table_count];
        memset(lt, 0, sizeof(*lt));

        const char* type_val = find_key(obj, "npc_type_id");
        if (type_val) lt->npc_type_id = (uint16_t)atoi(type_val);

        if (!parse_drops(lt, obj)) {
            free(lt->entries);
            memset(lt, 0, sizeof(*lt));
            free(obj);
            free(arr_json);
            return 0;
        }

        if (lt->npc_type_id > 0 && lt->entry_count > 0) {
            LOG_INFO("[LOOT] Loaded loot table for npc_type_id=%u (%d drops)",
                     lt->npc_type_id, lt->entry_count);
            g_loot_table_count++;
        } else {
            /* Nothing usable in this object; release what it allocated rather
             * than leaving it attached to a slot that will be overwritten. */
            free(lt->entries);
            memset(lt, 0, sizeof(*lt));
        }

        free(obj);
        pos = obj_end + 1;
        while (*pos && *pos != ',' && *pos != ']') pos++;
        if (*pos == ',') pos++;
    }

    free(arr_json);
    return 1;
}

/**
 * Initialize ground items and load NPC loot tables from JSON.
 *
 * @return 1 on success, or 0 when the file cannot be read or parsed.
 */
int loot_init(const char* json_path) {
    loot_cleanup();

    pthread_mutex_lock(&g_ground_items_lock);
    g_ground_items = calloc(GROUND_POOL_INITIAL, sizeof(*g_ground_items));
    if (!g_ground_items) {
        pthread_mutex_unlock(&g_ground_items_lock);
        LOG_ERROR("[LOOT] Out of memory allocating the ground-item pool");
        return 0;
    }
    g_ground_capacity     = GROUND_POOL_INITIAL;
    g_next_ground_item_id = 1;
    pthread_mutex_unlock(&g_ground_items_lock);

    char* json = read_file(json_path);
    if (!json) {
        LOG_ERROR("[LOOT] Failed to read %s", json_path);
        return 0;
    }

    int result = parse_loot_tables(json, json_path);
    free(json);

    LOG_INFO("[LOOT] Initialized with %d loot tables", g_loot_table_count);
    return result;
}

/**
 * Release every ground item and loot table.
 */
void loot_cleanup(void) {
    pthread_mutex_lock(&g_ground_items_lock);
    free(g_ground_items);
    g_ground_items    = NULL;
    g_ground_capacity = 0;
    pthread_mutex_unlock(&g_ground_items_lock);

    for (int i = 0; i < g_loot_table_count; i++) free(g_loot_tables[i].entries);
    free(g_loot_tables);
    g_loot_tables      = NULL;
    g_loot_table_count = 0;
    g_loot_table_cap   = 0;
    LOG_DEBUG("[LOOT] Cleaned up");
}

/* --- Dropping ------------------------------------------------------------ */

/** One drop staged inside the lock and broadcast outside it. */
typedef struct {
    uint32_t ground_id;
    uint32_t item_id;
    uint8_t  quantity;
    float    pos_x, pos_y;
} PendingDrop;

/**
 * Roll an NPC loot table and broadcast created ground items.
 *
 * @param npc_type_id  NPC type whose loot table is rolled; zero produces no drops.
 * @param x            Drop origin X coordinate in world units.
 * @param y            Drop origin Y coordinate in world units.
 * @param killer_id    Character receiving the exclusive-pickup window.
 * @return             The number of ground items created.
 */
int loot_roll(uint16_t npc_type_id, float x, float y, uint32_t killer_id) {
    if (npc_type_id == 0) return 0;

    const LootTable* lt = find_loot_table(npc_type_id);
    if (!lt || lt->entry_count <= 0) return 0;

    /* Sized to the table, so a table with more drops than some literal still
     * announces all of them. */
    PendingDrop* pending = calloc((size_t)lt->entry_count, sizeof(*pending));
    if (!pending) return 0;

    double now = get_time();
    int pending_count = 0;

    pthread_mutex_lock(&g_ground_items_lock);

    for (int e = 0; e < lt->entry_count; e++) {
        const LootEntry* entry = &lt->entries[e];

        float roll = (float)rand() / (float)RAND_MAX;
        if (roll > entry->drop_chance) continue;

        int slot = ground_claim_slot_locked();
        if (slot < 0) break;

        uint8_t qty = entry->min_qty;
        if (entry->max_qty > entry->min_qty)
            qty = (uint8_t)(entry->min_qty +
                            (rand() % (entry->max_qty - entry->min_qty + 1)));

        // Slight random offset so items don't stack visually
        float ox = ((float)(rand() % 40) - 20.0f);
        float oy = ((float)(rand() % 40) - 20.0f);

        GroundItem* gi = &g_ground_items[slot];
        memset(gi, 0, sizeof(*gi));
        gi->id        = g_next_ground_item_id++;
        gi->item_id   = entry->item_id;
        gi->quantity  = qty;
        gi->pos_x     = x + ox;
        gi->pos_y     = y + oy;
        gi->owner_id  = killer_id;
        gi->drop_time = now;
        gi->active    = 1;

        pending[pending_count].ground_id = gi->id;
        pending[pending_count].item_id   = gi->item_id;
        pending[pending_count].quantity  = gi->quantity;
        pending[pending_count].pos_x     = gi->pos_x;
        pending[pending_count].pos_y     = gi->pos_y;
        pending_count++;

        LOG_DEBUG("[LOOT] Dropped item %u (qty=%u) at (%.1f, %.1f) for killer %u",
                  entry->item_id, qty, gi->pos_x, gi->pos_y, killer_id);
    }

    pthread_mutex_unlock(&g_ground_items_lock);

    for (int p = 0; p < pending_count; p++)
        broadcast_drop(pending[p].ground_id, pending[p].item_id,
                       pending[p].quantity, pending[p].pos_x, pending[p].pos_y);

    free(pending);
    return pending_count;
}

/**
 * Create one ground item and broadcast it to nearby players.
 *
 * @param quantity  Stack quantity stored in the ground item.
 * @param x         Ground X coordinate in world units.
 * @param y         Ground Y coordinate in world units.
 * @param owner_id  Character receiving the exclusive-pickup window.
 * @return          The assigned ground-item identifier, or 0 on allocation failure.
 */
uint32_t loot_drop_item(uint32_t item_id, uint8_t quantity, float x, float y,
                        uint32_t owner_id) {
    double now = get_time();
    uint32_t ground_id = 0;

    pthread_mutex_lock(&g_ground_items_lock);

    int slot = ground_claim_slot_locked();
    if (slot >= 0) {
        GroundItem* gi = &g_ground_items[slot];
        memset(gi, 0, sizeof(*gi));
        gi->id        = g_next_ground_item_id++;
        gi->item_id   = item_id;
        gi->quantity  = quantity;
        gi->pos_x     = x;
        gi->pos_y     = y;
        gi->owner_id  = owner_id;
        gi->drop_time = now;
        gi->active    = 1;
        ground_id     = gi->id;
    }

    pthread_mutex_unlock(&g_ground_items_lock);

    if (ground_id == 0) {
        LOG_WARN_RL(5, 60, "[LOOT] Cannot place item %u on the ground", item_id);
        return 0;
    }

    broadcast_drop(ground_id, item_id, quantity, x, y);

    LOG_DEBUG("[LOOT] Player %u dropped item %u (qty=%u) at (%.1f, %.1f) ground_id=%u",
              owner_id, item_id, quantity, x, y, ground_id);
    return ground_id;
}

/* --- Reading ------------------------------------------------------------- */

/**
 * Copy one ground item out under the pool lock.
 *
 * @return 1 when `out` was filled, or 0 when no active item carries that id.
 */
int loot_snapshot(uint32_t ground_item_id, GroundItem* out) {
    if (!out) return 0;

    pthread_mutex_lock(&g_ground_items_lock);
    const GroundItem* gi = ground_find_locked(ground_item_id);
    if (gi) *out = *gi;
    pthread_mutex_unlock(&g_ground_items_lock);

    return gi != NULL;
}

/** Ground items currently on the world floor. */
size_t loot_active_count(void) {
    size_t n = 0;
    pthread_mutex_lock(&g_ground_items_lock);
    for (size_t i = 0; i < g_ground_capacity; i++)
        if (g_ground_items[i].active) n++;
    pthread_mutex_unlock(&g_ground_items_lock);
    return n;
}

/** Slots the ground-item pool has grown to hold. */
size_t loot_pool_capacity(void) {
    pthread_mutex_lock(&g_ground_items_lock);
    size_t cap = g_ground_capacity;
    pthread_mutex_unlock(&g_ground_items_lock);
    return cap;
}

/** Loot tables loaded from JSON. */
int loot_table_count(void) {
    return g_loot_table_count;
}

/* --- Reserve, commit, restore -------------------------------------------- */

/**
 * Claim a ground item for a player without destroying it.
 *
 * @return 1 when reserved, or 0 when absent, already reserved, or still inside
 *         another player's exclusive window.
 */
int loot_reserve(uint32_t ground_item_id, uint32_t player_id,
                 uint32_t* out_item_id, uint8_t* out_quantity) {
    double now = get_time();
    int reserved = 0;

    pthread_mutex_lock(&g_ground_items_lock);

    GroundItem* gi = ground_find_locked(ground_item_id);
    if (gi) {
        int held_by_other = gi->reserved_by != 0 &&
                            gi->reserved_by != player_id &&
                            (now - gi->reserved_at) < LOOT_RESERVATION_TIMEOUT;

        /* An owner of 0 means the drop belongs to nobody -- a world spawn, or
         * an item placed by the server. Comparing it to a character id the way
         * this used to locked every player out of such a drop for the full
         * exclusive window, on the strength of "0 is not you". */
        double elapsed = now - gi->drop_time;
        int inside_owner_window = gi->owner_id != 0 &&
                                  elapsed < LOOT_OWNER_TIME &&
                                  gi->owner_id != player_id;

        if (!held_by_other && !inside_owner_window) {
            gi->reserved_by = player_id;
            gi->reserved_at = now;
            if (out_item_id)  *out_item_id  = gi->item_id;
            if (out_quantity) *out_quantity = gi->quantity;
            reserved = 1;
        }
    }

    pthread_mutex_unlock(&g_ground_items_lock);
    return reserved;
}

/**
 * Finish a reservation: the item leaves the world and nearby players are told.
 */
void loot_commit(uint32_t ground_item_id) {
    float x = 0.0f, y = 0.0f;
    int removed = 0;

    pthread_mutex_lock(&g_ground_items_lock);
    GroundItem* gi = ground_find_locked(ground_item_id);
    if (gi) {
        x = gi->pos_x;
        y = gi->pos_y;
        gi->active      = 0;
        gi->reserved_by = 0;
        removed = 1;
    }
    pthread_mutex_unlock(&g_ground_items_lock);

    /* Addressed by position rather than sent to everyone online, matching what
     * loot_tick() already did for expiry. At capacity the old full-world send
     * was one packet per online player for every pickup in the world, nearly
     * all of them naming an item the recipient had never been shown. */
    if (removed) broadcast_despawn(ground_item_id, x, y);
}

/**
 * Abandon a reservation: the item returns to the ground, unclaimed.
 */
void loot_restore(uint32_t ground_item_id, uint32_t player_id) {
    pthread_mutex_lock(&g_ground_items_lock);
    GroundItem* gi = ground_find_locked(ground_item_id);
    /* Only the holder may release it: a late restore must not free a
     * reservation the timeout sweep already handed to somebody else. */
    if (gi && gi->reserved_by == player_id) gi->reserved_by = 0;
    pthread_mutex_unlock(&g_ground_items_lock);
}

/* --- Pickup -------------------------------------------------------------- */

/** Answer one pickup request. */
static void send_pickup_response(int client_fd, uint32_t character_id,
                                 uint32_t ground_item_id, int success,
                                 uint32_t item_id, uint8_t quantity,
                                 uint8_t inventory_slot, const char* message) {
    LootPickupResponsePacket resp = {0};
    resp.header.type         = PACKET_LOOT_PICKUP_RESPONSE;
    resp.header.player_id    = htonl(character_id);
    resp.header.payload_size = htons(sizeof(resp) - sizeof(PacketHeader));
    resp.ground_item_id      = htonl(ground_item_id);
    resp.success             = (uint8_t)(success ? 1 : 0);
    resp.item_id             = htonl(item_id);
    resp.quantity            = quantity;
    resp.inventory_slot      = inventory_slot;
    snprintf(resp.message, sizeof(resp.message), "%s", message);

    server_send(client_fd, &resp, sizeof(resp));
}

/**
 * Handle one LOOT_PICKUP_REQUEST end to end.
 *
 * Reserve, insert, then commit. The previous order -- deactivate the item, then
 * try to insert it -- destroyed the item whenever the insert stored nothing,
 * and the player was told "Player state changed" while their loot ceased to
 * exist.
 */
void loot_handle_pickup_request(uint32_t character_id, int client_fd,
                                const uint8_t* buffer, ssize_t bytes) {
    if (bytes < (ssize_t)sizeof(LootPickupRequestPacket)) {
        LOG_WARN_RL(5, 60, "[LOOT] Malformed pickup request (size: %zd)", bytes);
        return;
    }

    const LootPickupRequestPacket* req = (const LootPickupRequestPacket*)buffer;
    uint32_t ground_item_id = ntohl(req->ground_item_id);

    GroundItem snapshot;
    if (!loot_snapshot(ground_item_id, &snapshot)) {
        send_pickup_response(client_fd, character_id, ground_item_id, 0, 0, 0, 0,
                             "Item not found");
        return;
    }

    ActivePlayer* player = player_acquire(character_id);
    if (!player) return;

    float dx = player->pos_x - snapshot.pos_x;
    float dy = player->pos_y - snapshot.pos_y;
    float dist = sqrtf(dx * dx + dy * dy);
    int inv_slot = inventory_first_free(player->inventory);
    player_release(player);

    if (dist > LOOT_PICKUP_RANGE) {
        send_pickup_response(client_fd, character_id, ground_item_id, 0, 0, 0, 0,
                             "Too far away");
        return;
    }

    if (inv_slot < 0) {
        send_pickup_response(client_fd, character_id, ground_item_id, 0, 0, 0, 0,
                             "Inventory full");
        return;
    }

    uint32_t item_id = 0;
    uint8_t  quantity = 0;
    if (!loot_reserve(ground_item_id, character_id, &item_id, &quantity)) {
        send_pickup_response(client_fd, character_id, ground_item_id, 0, 0, 0, 0,
                             "Cannot pick up yet");
        return;
    }

    /* The item is reserved from here on. Every path out of this block either
     * commits it or restores it; none of them may return without doing one. */
    uint16_t stored = 0;
    player = player_acquire(character_id);
    if (player && player->client_fd == client_fd) {
        const ItemDefinition* def = item_get(item_id);
        uint16_t want = quantity ? quantity : 1;
        uint16_t left = inventory_add(player->inventory, item_id, want,
                                      def ? def->max_stack : 1,
                                      def ? def->bind_on_pickup : 0);
        stored = (uint16_t)(want - left);
        if (stored > 0) {
            /* Rarity is the line. Every kill drops something, and marking each
             * of them critical would turn the milestone pass into a second full
             * sweep running 24x as often -- which is the ordinary sweep with
             * extra steps. An uncommon-or-better drop is rare enough to be
             * worth a write of its own and rare enough that doing so costs
             * nothing. */
            if (def && def->rarity >= RARITY_UNCOMMON) player_mark_critical(player);
            else                                       player->is_dirty = 1;
        }
    }
    if (player) player_release(player);

    if (stored == 0) {
        /* Nothing was taken, so nothing may be destroyed. The item goes back on
         * the ground for this player or anyone else to try again. */
        loot_restore(ground_item_id, character_id);
        send_pickup_response(client_fd, character_id, ground_item_id, 0, 0, 0, 0,
                             "Player state changed");
        return;
    }

    loot_commit(ground_item_id);

    quest_on_item_collect(character_id, client_fd, item_id);

    send_pickup_response(client_fd, character_id, ground_item_id, 1, item_id,
                         (uint8_t)stored, (uint8_t)inv_slot, "Item picked up");

    // refresh the stack that may have absorbed the pickup
    uint16_t changed[1] = { (uint16_t)inv_slot };
    player_send_slot_updates(client_fd, character_id, changed, 1);
}

/* --- The tick ------------------------------------------------------------ */

/**
 * Despawn expired ground items and tell the players near each one.
 *
 * @param players  The pass's player snapshot; NULL expires items silently.
 */
void loot_tick(TickSnapshot* players) {
    double now = get_time();

    /* Sized to the pool, not to a round number.
     *
     * This was uint32_t[32] with a `if (despawn_count < 32)` guard around the
     * record -- but the item was deactivated either way. Past the 32nd expiry in
     * one tick the item was gone on the server and the client was never told, so
     * it kept drawing loot that no longer existed and could not be picked up.
     * Expiries are correlated, not random: everything a pack dropped was dropped
     * within seconds of everything else and expires together, which is exactly
     * the case that overran 32.
     */
    struct Despawned { uint32_t id; float x, y; };
    struct Despawned* despawned = NULL;
    size_t despawn_count = 0;

    pthread_mutex_lock(&g_ground_items_lock);

    if (g_ground_capacity > 0) {
        despawned = malloc(g_ground_capacity * sizeof(*despawned));
        if (despawned) {
            for (size_t i = 0; i < g_ground_capacity; i++) {
                GroundItem* gi = &g_ground_items[i];
                if (!gi->active) continue;

                /* A reservation nobody resolved goes back to the ground before
                 * anything else looks at the item. */
                if (gi->reserved_by != 0 &&
                    (now - gi->reserved_at) >= LOOT_RESERVATION_TIMEOUT) {
                    LOG_WARN_RL(5, 60,
                                "[LOOT] Reservation on ground item %u by character %u "
                                "expired unresolved; returning it to the ground",
                                gi->id, gi->reserved_by);
                    gi->reserved_by = 0;
                }

                if ((now - gi->drop_time) < LOOT_DESPAWN_TIME) continue;

                /* An item mid-pickup is left alone: the pickup owns it, and
                 * expiring it underneath would let the same stack be both
                 * inserted and despawned. It expires on the next pass once the
                 * reservation is resolved or times out. */
                if (gi->reserved_by != 0) continue;

                despawned[despawn_count].id = gi->id;
                despawned[despawn_count].x  = gi->pos_x;
                despawned[despawn_count].y  = gi->pos_y;
                despawn_count++;
                gi->active = 0;
            }
        } else {
            LOG_ERROR("[LOOT] Out of memory during the despawn pass");
        }
    }

    pthread_mutex_unlock(&g_ground_items_lock);

    if (despawn_count == 0 || !players) { free(despawned); return; }

    /* Addressed by position rather than sent to everyone online. The previous
     * version sent every despawn to every player in the world: at capacity that
     * was 256 items x 1,000 players = 256,000 packets from one tick, of which
     * all but a handful named an item the recipient had never been shown. */
    /* Sized to the player table, hoisted out of the loop and reused across the
     * pass. This was [64], which was not a view radius but a lost notification:
     * the 65th player near an expiring item was never told it was gone, so their
     * client drew loot that no longer existed and could not be picked up, for
     * the rest of the session. Everyone in range is a real recipient. */
    int* nearby = malloc(sizeof(int) * MAX_PLAYERS);
    if (!nearby) { free(despawned); return; }

    for (size_t d = 0; d < despawn_count; d++) {
        LootDespawnPacket pkt = {0};
        pkt.header.type         = PACKET_LOOT_DESPAWN;
        pkt.header.player_id    = 0;
        pkt.header.payload_size = htons(sizeof(LootDespawnPacket) - sizeof(PacketHeader));
        pkt.ground_item_id = htonl(despawned[d].id);

        int n = tick_snapshot_query(players, despawned[d].x, despawned[d].y,
                                    LOOT_VIEW_RADIUS, nearby, MAX_PLAYERS);
        for (int k = 0; k < n; k++)
            server_send(players->client_fd[nearby[k]], &pkt, sizeof(pkt));
    }

    free(nearby);
    free(despawned);
}
