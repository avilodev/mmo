/** @file Define loot tables and the world server's ground-item lifecycle. */

#ifndef LOOT_H
#define LOOT_H

#include <stdint.h>

/** Bound loot records and configure ownership, despawn, and pickup limits. */
#define MAX_LOOT_ENTRIES   8    /**< Maximum drops for one NPC type. */
#define MAX_LOOT_TABLES    64   /**< Maximum distinct NPC loot tables. */
#define MAX_GROUND_ITEMS   256  /**< Maximum simultaneous ground items. */
#define LOOT_OWNER_TIME    30.0 /**< Exclusive-owner duration in seconds. */
#define LOOT_DESPAWN_TIME  120.0 /**< Ground-item lifetime in seconds. */
#define LOOT_PICKUP_RANGE  80.0f /**< Maximum pickup distance in world units. */

/** Define one probabilistic item and quantity range in a loot table. */
typedef struct {
    uint32_t item_id;
    float    drop_chance;       // 0.0 - 1.0
    uint8_t  min_qty;
    uint8_t  max_qty;
} LootEntry;

/** Associate a bounded set of loot entries with one NPC type. */
typedef struct {
    uint16_t npc_type_id;
    int      entry_count;
    LootEntry entries[MAX_LOOT_ENTRIES];
} LootTable;

/** Track one dropped item through ownership, pickup, or despawn. */
typedef struct {
    uint32_t id;                // Unique ground item ID
    uint32_t item_id;           // Item definition ID
    uint8_t  quantity;
    float    pos_x, pos_y;
    uint32_t owner_id;          // Killer's character_id (exclusive pickup)
    double   drop_time;         // When it was dropped (CLOCK_MONOTONIC)
    uint8_t  active;            // 1 = on ground, 0 = picked up or despawned
} GroundItem;

// parse loot_tables from the item JSON registry
int  loot_init(const char* json_path);
void loot_cleanup(void);

// return the number of spawned drops after notifying nearby players
int  loot_roll(uint16_t npc_type_id, float x, float y, uint32_t killer_id);

// fill outputs and return nonzero only after a successful pickup
int  loot_try_pickup(uint32_t ground_item_id, uint32_t player_id,
                     uint32_t* out_item_id, uint8_t* out_quantity);

void loot_tick(void);

// return a new ground identifier or zero after spawn failure
uint32_t loot_drop_item(uint32_t item_id, uint8_t quantity, float x, float y, uint32_t owner_id);

// return a system-owned active item or NULL when absent
const GroundItem* loot_get_ground_item(uint32_t ground_item_id);

#endif // LOOT_H
