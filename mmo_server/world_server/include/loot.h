// ============================================================================
// loot.h — Loot table loading, ground item management, and pickup logic
// ============================================================================

#ifndef LOOT_H
#define LOOT_H

#include <stdint.h>

// ---------------------------------------------------------------------------
// Loot table definitions (loaded from items.json "loot_tables" section)
// ---------------------------------------------------------------------------

#define MAX_LOOT_ENTRIES   8    // Max drops per NPC type
#define MAX_LOOT_TABLES    64   // Max distinct NPC loot tables
#define MAX_GROUND_ITEMS   256  // Max items on the ground at once
#define LOOT_OWNER_TIME    30.0 // Seconds before loot becomes free-for-all
#define LOOT_DESPAWN_TIME  120.0 // Seconds before ground items disappear
#define LOOT_PICKUP_RANGE  80.0f // Max distance to pick up a ground item

typedef struct {
    uint32_t item_id;
    float    drop_chance;       // 0.0 - 1.0
    uint8_t  min_qty;
    uint8_t  max_qty;
} LootEntry;

typedef struct {
    uint16_t npc_type_id;
    int      entry_count;
    LootEntry entries[MAX_LOOT_ENTRIES];
} LootTable;

// ---------------------------------------------------------------------------
// Ground items — items sitting on the ground waiting to be picked up
// ---------------------------------------------------------------------------

typedef struct {
    uint32_t id;                // Unique ground item ID
    uint32_t item_id;           // Item definition ID
    uint8_t  quantity;
    float    pos_x, pos_y;
    uint32_t owner_id;          // Killer's character_id (exclusive pickup)
    double   drop_time;         // When it was dropped (CLOCK_MONOTONIC)
    uint8_t  active;            // 1 = on ground, 0 = picked up or despawned
} GroundItem;

// ---------------------------------------------------------------------------
// API
// ---------------------------------------------------------------------------

// Initialize loot system — parse loot_tables from items.json
int  loot_init(const char* json_path);
void loot_cleanup(void);

// Roll loot for an NPC type at a position. Returns number of items dropped.
// Sends LOOT_DROP packets to nearby players.
int  loot_roll(uint16_t npc_type_id, float x, float y, uint32_t killer_id);

// Try to pick up a ground item. Returns 1 on success, 0 on failure.
// On success, fills out_item_id, out_quantity.
int  loot_try_pickup(uint32_t ground_item_id, uint32_t player_id,
                     uint32_t* out_item_id, uint8_t* out_quantity);

// Tick ground items — despawn expired ones. Call from combat tick.
void loot_tick(void);

// Spawn a single item on the ground (for player drops). Returns ground_item_id, 0 on failure.
// Sends LOOT_DROP packets to nearby players.
uint32_t loot_drop_item(uint32_t item_id, uint8_t quantity, float x, float y, uint32_t owner_id);

// Get a ground item by ID (for distance checks). Returns NULL if not found.
const GroundItem* loot_get_ground_item(uint32_t ground_item_id);

#endif // LOOT_H
