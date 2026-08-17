/**
 * @file
 * Load NPC loot tables and manage world-server ground-item lifetimes and pickup.
 */

#include "loot.h"
#include "log.h"
#include "types.h"
#include "player_data.h"

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

static LootTable      g_loot_tables[MAX_LOOT_TABLES];
static int            g_loot_table_count = 0;

static GroundItem     g_ground_items[MAX_GROUND_ITEMS];
static pthread_mutex_t g_ground_items_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t       g_next_ground_item_id = 1;

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
    char* buf = malloc(size + 1);
    if (!buf) { fclose(f); return NULL; }
    fread(buf, 1, size, f);
    buf[size] = '\0';
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

/**
 * Parse loot-table entries into the fixed registry.
 *
 * @return 1 when parsing completes or no section exists, or 0 on malformed input or allocation failure.
 */
static int parse_loot_tables(const char* json) {
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
    int arr_len = (int)(arr_end - arr_start + 1);
    char* arr_json = malloc(arr_len + 1);
    if (!arr_json) {
        LOG_ERROR("[LOOT] malloc failed while parsing loot_tables");
        return 0;
    }
    memcpy(arr_json, arr_start, arr_len);
    arr_json[arr_len] = '\0';

    // Parse each loot table object
    const char* pos = arr_json + 1; // skip '['
    while (*pos && g_loot_table_count < MAX_LOOT_TABLES) {
        pos = skip_ws(pos);
        if (*pos == ']') break;
        if (*pos != '{') { pos++; continue; }

        const char* obj_end = find_matching(pos, '{', '}');
        if (!obj_end) break;

        int obj_len = (int)(obj_end - pos + 1);
        char* obj = malloc(obj_len + 1);
        if (!obj) {
            LOG_ERROR("[LOOT] malloc failed while parsing loot table object");
            free(arr_json);
            return 0;
        }
        memcpy(obj, pos, obj_len);
        obj[obj_len] = '\0';

        LootTable* lt = &g_loot_tables[g_loot_table_count];
        memset(lt, 0, sizeof(LootTable));

        // Parse npc_type_id
        const char* type_val = find_key(obj, "npc_type_id");
        if (type_val) {
            lt->npc_type_id = (uint16_t)atoi(type_val);
        }

        // Parse drops array
        const char* drops_start = strstr(obj, "\"drops\"");
        if (drops_start) {
            const char* drops_arr = strchr(drops_start, '[');
            if (drops_arr) {
                const char* drops_end = find_matching(drops_arr, '[', ']');
                if (drops_end) {
                    const char* dp = drops_arr + 1;
                    while (*dp && lt->entry_count < MAX_LOOT_ENTRIES) {
                        dp = skip_ws(dp);
                        if (*dp == ']') break;
                        if (*dp != '{') { dp++; continue; }

                        const char* de = find_matching(dp, '{', '}');
                        if (!de) break;

                        int de_len = (int)(de - dp + 1);
                        char* drop_obj = malloc(de_len + 1);
                        if (!drop_obj) {
                            LOG_ERROR("[LOOT] malloc failed while parsing drop entry");
                            free(obj);
                            free(arr_json);
                            return 0;
                        }
                        memcpy(drop_obj, dp, de_len);
                        drop_obj[de_len] = '\0';

                        LootEntry* entry = &lt->entries[lt->entry_count];

                        const char* v;
                        v = find_key(drop_obj, "item_id");
                        if (v) entry->item_id = (uint32_t)atoi(v);

                        v = find_key(drop_obj, "chance");
                        if (v) entry->drop_chance = (float)atof(v);

                        v = find_key(drop_obj, "min_qty");
                        if (v) entry->min_qty = (uint8_t)atoi(v);
                        else entry->min_qty = 1;

                        v = find_key(drop_obj, "max_qty");
                        if (v) entry->max_qty = (uint8_t)atoi(v);
                        else entry->max_qty = 1;

                        lt->entry_count++;
                        free(drop_obj);

                        dp = de + 1;
                        while (*dp && *dp != ',' && *dp != ']') dp++;
                        if (*dp == ',') dp++;
                    }
                }
            }
        }

        if (lt->npc_type_id > 0 && lt->entry_count > 0) {
            LOG_INFO("[LOOT] Loaded loot table for npc_type_id=%u (%d drops)", lt->npc_type_id, lt->entry_count);
            g_loot_table_count++;
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
    memset(g_ground_items, 0, sizeof(g_ground_items));
    g_loot_table_count = 0;
    g_next_ground_item_id = 1;

    char* json = read_file(json_path);
    if (!json) {
        LOG_ERROR("[LOOT] Failed to read %s", json_path);
        return 0;
    }

    int result = parse_loot_tables(json);
    free(json);

    LOG_INFO("[LOOT] Initialized with %d loot tables", g_loot_table_count);
    return result;
}

/**
 * Clear all ground items and loaded loot tables.
 */
void loot_cleanup(void) {
    pthread_mutex_lock(&g_ground_items_lock);
    memset(g_ground_items, 0, sizeof(g_ground_items));
    pthread_mutex_unlock(&g_ground_items_lock);
    g_loot_table_count = 0;
    LOG_DEBUG("[LOOT] Cleaned up");
}

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
    if (!lt) return 0;

    double now = get_time();
    int dropped = 0;

    // Collect ground items to create
    typedef struct {
        uint32_t ground_id;
        uint32_t item_id;
        uint8_t  quantity;
        float    pos_x, pos_y;
    } PendingDrop;

    PendingDrop pending[MAX_LOOT_ENTRIES];
    int pending_count = 0;

    pthread_mutex_lock(&g_ground_items_lock);

    for (int e = 0; e < lt->entry_count; e++) {
        const LootEntry* entry = &lt->entries[e];

        // Roll drop chance
        float roll = (float)rand() / (float)RAND_MAX;
        if (roll > entry->drop_chance) continue;

        // Find a free slot
        int slot = -1;
        for (int i = 0; i < MAX_GROUND_ITEMS; i++) {
            if (!g_ground_items[i].active) { slot = i; break; }
        }
        if (slot == -1) {
            LOG_WARN_RL(5, 60, "[LOOT] Ground item pool full!");
            break;
        }

        // Determine quantity
        uint8_t qty = entry->min_qty;
        if (entry->max_qty > entry->min_qty) {
            qty = entry->min_qty + (rand() % (entry->max_qty - entry->min_qty + 1));
        }

        // Slight random offset so items don't stack visually
        float ox = ((float)(rand() % 40) - 20.0f);
        float oy = ((float)(rand() % 40) - 20.0f);

        GroundItem* gi = &g_ground_items[slot];
        gi->id = g_next_ground_item_id++;
        gi->item_id = entry->item_id;
        gi->quantity = qty;
        gi->pos_x = x + ox;
        gi->pos_y = y + oy;
        gi->owner_id = killer_id;
        gi->drop_time = now;
        gi->active = 1;

        if (pending_count < MAX_LOOT_ENTRIES) {
            pending[pending_count].ground_id = gi->id;
            pending[pending_count].item_id = gi->item_id;
            pending[pending_count].quantity = gi->quantity;
            pending[pending_count].pos_x = gi->pos_x;
            pending[pending_count].pos_y = gi->pos_y;
            pending_count++;
        }

        dropped++;
        LOG_DEBUG("[LOOT] Dropped item %u (qty=%u) at (%.1f, %.1f) for killer %u", entry->item_id, qty, gi->pos_x, gi->pos_y, killer_id);
    }

    pthread_mutex_unlock(&g_ground_items_lock);

    // Broadcast LOOT_DROP to nearby players (outside lock)
    if (pending_count > 0) {
        // Snapshot nearby player fds
        typedef struct { int fd; } PlayerFd;
        PlayerFd fds[64];
        int fd_count = 0;

        player_registry_rdlock();
        int online_count = 0;
        const int* online = player_active_list_locked(&online_count);
        for (int n = 0; n < online_count && fd_count < 64; n++) {
            int i = online[n];
            if (!active_players[i].is_loaded) continue;
            float dx = active_players[i].pos_x - x;
            float dy = active_players[i].pos_y - y;
            if (dx * dx + dy * dy <= 500.0f * 500.0f) {
                fds[fd_count++].fd = active_players[i].client_fd;
            }
        }
        player_registry_unlock();

        for (int p = 0; p < pending_count; p++) {
            LootDropPacket pkt = {0};
            pkt.header.type         = PACKET_LOOT_DROP;
            pkt.header.player_id    = 0;
            pkt.header.payload_size = htons(sizeof(LootDropPacket) - sizeof(PacketHeader));
            pkt.ground_item_id = htonl(pending[p].ground_id);
            pkt.item_id = htonl(pending[p].item_id);
            pkt.quantity = pending[p].quantity;
            pkt.pos_x = pending[p].pos_x;
            pkt.pos_y = pending[p].pos_y;

            for (int f = 0; f < fd_count; f++) {
                server_send(fds[f].fd, &pkt, sizeof(pkt));
            }
        }
    }

    return dropped;
}

/**
 * Create one ground item and broadcast it to nearby players.
 *
 * @param quantity  Stack quantity stored in the ground item.
 * @param x         Ground X coordinate in world units.
 * @param y         Ground Y coordinate in world units.
 * @param owner_id  Character receiving the exclusive-pickup window.
 * @return          The assigned ground-item identifier, or 0 when the pool is full.
 */
uint32_t loot_drop_item(uint32_t item_id, uint8_t quantity, float x, float y, uint32_t owner_id) {
    double now = get_time();
    uint32_t ground_id = 0;

    pthread_mutex_lock(&g_ground_items_lock);

    int slot = -1;
    for (int i = 0; i < MAX_GROUND_ITEMS; i++) {
        if (!g_ground_items[i].active) { slot = i; break; }
    }

    if (slot >= 0) {
        GroundItem* gi = &g_ground_items[slot];
        gi->id = g_next_ground_item_id++;
        gi->item_id = item_id;
        gi->quantity = quantity;
        gi->pos_x = x;
        gi->pos_y = y;
        gi->owner_id = owner_id;
        gi->drop_time = now;
        gi->active = 1;
        ground_id = gi->id;
    }

    pthread_mutex_unlock(&g_ground_items_lock);

    if (ground_id == 0) {
        LOG_WARN_RL(5, 60, "[LOOT] Ground item pool full, cannot drop item %u", item_id);
        return 0;
    }

    // Broadcast LOOT_DROP to nearby players
    LootDropPacket pkt = {0};
    pkt.header.type         = PACKET_LOOT_DROP;
    pkt.header.payload_size = htons(sizeof(LootDropPacket) - sizeof(PacketHeader));
    pkt.ground_item_id = htonl(ground_id);
    pkt.item_id = htonl(item_id);
    pkt.quantity = quantity;
    pkt.pos_x = x;
    pkt.pos_y = y;

    player_registry_rdlock();
    int online_count = 0;
    const int* online = player_active_list_locked(&online_count);
    for (int n = 0; n < online_count; n++) {
        int i = online[n];
        if (!active_players[i].is_loaded) continue;
        float dx = active_players[i].pos_x - x;
        float dy = active_players[i].pos_y - y;
        if (dx * dx + dy * dy <= 500.0f * 500.0f) {
            server_send(active_players[i].client_fd, &pkt, sizeof(pkt));
        }
    }
    player_registry_unlock();

    LOG_DEBUG("[LOOT] Player %u dropped item %u (qty=%u) at (%.1f, %.1f) ground_id=%u", owner_id, item_id, quantity, x, y, ground_id);
    return ground_id;
}

/**
 * Claim a ground item after enforcing its ownership window.
 *
 * @param out_item_id   Receives the claimed item definition identifier.
 * @param out_quantity  Receives the claimed stack quantity.
 * @return              1 when claimed, or 0 when absent or still reserved for another player.
 */
int loot_try_pickup(uint32_t ground_item_id, uint32_t player_id,
                    uint32_t* out_item_id, uint8_t* out_quantity) {
    double now = get_time();
    int success = 0;

    pthread_mutex_lock(&g_ground_items_lock);

    for (int i = 0; i < MAX_GROUND_ITEMS; i++) {
        GroundItem* gi = &g_ground_items[i];
        if (!gi->active || gi->id != ground_item_id) continue;

        // Check ownership timer
        double elapsed = now - gi->drop_time;
        if (elapsed < LOOT_OWNER_TIME && gi->owner_id != player_id) {
            // Not the owner and still in exclusive window
            pthread_mutex_unlock(&g_ground_items_lock);
            return 0;
        }

        *out_item_id = gi->item_id;
        *out_quantity = gi->quantity;
        gi->active = 0; // Remove from ground

        success = 1;
        break;
    }

    pthread_mutex_unlock(&g_ground_items_lock);

    // If picked up, broadcast despawn to nearby players
    if (success) {
        LootDespawnPacket pkt = {0};
        pkt.header.type         = PACKET_LOOT_DESPAWN;
        pkt.header.player_id    = 0;
        pkt.header.payload_size = htons(sizeof(LootDespawnPacket) - sizeof(PacketHeader));
        pkt.ground_item_id = htonl(ground_item_id);

        player_registry_rdlock();
        int online_count = 0;
        const int* online = player_active_list_locked(&online_count);
        for (int n = 0; n < online_count; n++) {
            int i = online[n];
            if (!active_players[i].is_loaded) continue;
            server_send(active_players[i].client_fd, &pkt, sizeof(pkt));
        }
        player_registry_unlock();
    }

    return success;
}

/**
 * Despawn expired ground items and broadcast their removal.
 */
void loot_tick(void) {
    double now = get_time();

    // Collect despawned IDs under lock
    uint32_t despawned[32];
    int despawn_count = 0;

    pthread_mutex_lock(&g_ground_items_lock);
    for (int i = 0; i < MAX_GROUND_ITEMS; i++) {
        if (!g_ground_items[i].active) continue;
        if ((now - g_ground_items[i].drop_time) >= LOOT_DESPAWN_TIME) {
            if (despawn_count < 32) {
                despawned[despawn_count++] = g_ground_items[i].id;
            }
            g_ground_items[i].active = 0;
        }
    }
    pthread_mutex_unlock(&g_ground_items_lock);

    // Broadcast despawns
    if (despawn_count > 0) {
        // reuse one recipient snapshot for all expirations
        int recipients[MAX_PLAYERS];
        int recipient_count = 0;

        player_registry_rdlock();
        int online_count = 0;
        const int* online = player_active_list_locked(&online_count);
        for (int n = 0; n < online_count; n++) {
            int i = online[n];
            if (!active_players[i].is_loaded) continue;
            recipients[recipient_count++] = active_players[i].client_fd;
        }
        player_registry_unlock();

        for (int d = 0; d < despawn_count; d++) {
            LootDespawnPacket pkt = {0};
            pkt.header.type         = PACKET_LOOT_DESPAWN;
            pkt.header.player_id    = 0;
            pkt.header.payload_size = htons(sizeof(LootDespawnPacket) - sizeof(PacketHeader));
            pkt.ground_item_id = htonl(despawned[d]);

            for (int r = 0; r < recipient_count; r++)
                server_send(recipients[r], &pkt, sizeof(pkt));
        }
    }
}

/**
 * Retrieve a ground item by identifier without locking its pool.
 *
 * The returned pool pointer may become stale immediately; callers must tolerate concurrent changes.
 *
 * @return The active ground item, or NULL when absent.
 */
const GroundItem* loot_get_ground_item(uint32_t ground_item_id) {
    for (int i = 0; i < MAX_GROUND_ITEMS; i++) {
        if (g_ground_items[i].active && g_ground_items[i].id == ground_item_id) {
            return &g_ground_items[i];
        }
    }
    return NULL;
}
