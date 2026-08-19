#ifndef ITEM_DEFINITIONS_H
#define ITEM_DEFINITIONS_H

#include "protocol.h"

#include <stdint.h>

/** Bound loaded item definitions and their display names. */
#define MAX_ITEMS 100000
#define MAX_ITEM_NAME 64

/** Identify an item's gameplay category. */
typedef enum {
    ITEM_TYPE_ITEM = 0,        // Generic items (crafting materials, etc)
    ITEM_TYPE_WEAPON = 1,      // Weapons
    ITEM_TYPE_SHIELD = 2,      // Shields
    ITEM_TYPE_ARMOR = 3,       // Armor pieces
    ITEM_TYPE_CONSUMABLE = 4,  // Potions, food, etc
    ITEM_TYPE_QUEST = 5        // Quest items
} ItemType;

/** Identify resource changes produced by consumable use. */
typedef enum {
    USE_EFFECT_NONE = 0,
    USE_EFFECT_RESTORE_HEALTH = 1,
    USE_EFFECT_RESTORE_MANA = 2,
    USE_EFFECT_RESTORE_BOTH = 3,
} UseEffectType;

/** Order item rarity tiers used by presentation and loot data. */
typedef enum {
    RARITY_COMMON = 0,
    RARITY_UNCOMMON = 1,
    RARITY_RARE = 2,
    RARITY_EPIC = 3,
    RARITY_LEGENDARY = 4
} ItemRarity;

/** Item requirements name races by their registry identifier.
 *
 * Race and class fuse into one identifier, so class_req and race_req below are two
 * views of the same thing. Both are kept because an item may sensibly restrict on
 * either, and both are matched against the character's single race identifier.
 */

/** Aggregate immutable JSON-backed properties for one item type. */
typedef struct {
    uint32_t id;
    char name[MAX_ITEM_NAME];

    ItemType type;
    EquipSlotType slot;

    uint32_t damage;           // For weapons
    uint32_t defense;          // For armor/shields

    /** Hold attribute bonuses indexed by StatId, keyed in JSON by stat name.
     *
     * An array rather than named fields, for the same reason the wire carries one:
     * adding a stat is one enum entry and one JSON key, not an edit to every struct
     * that mentions attributes. */
    int32_t bonus_stats[STAT_COUNT];

    uint8_t level_req;         // Minimum level required
    uint8_t class_req[5];      // Required classes (0 = any, otherwise specific class IDs)
    uint8_t race_req[4];       // Required races (0 = any)

    ItemRarity rarity;
    uint8_t craftable;         // 0 = no, 1 = yes
    uint8_t stackable;         // 0 = no, 1 = yes
    uint16_t max_stack;        // Maximum stack size
    uint32_t value;            // Gold value (for selling)

    uint8_t use_effect;        // UseEffectType — what happens when used
    int32_t use_value;         // How much to restore/apply
    float   use_cooldown;      // Seconds before this consumable can be used again

    uint8_t is_two_handed;     // 1 if two-handed weapon
    uint8_t bind_on_pickup;    // 1 if binds when picked up
    uint8_t bind_on_equip;     // 1 if binds when equipped
} ItemDefinition;

// load once during world-server startup
int items_init(const char* json_filepath);

// return a registry-owned definition or NULL when absent
const ItemDefinition* item_get(uint32_t item_id);

int item_exists(uint32_t item_id);

int item_can_equip(uint32_t item_id, uint8_t character_level,
                   uint8_t character_class, uint8_t character_race);

int items_get_count(void);

void items_cleanup(void);

const char* class_get_name(uint8_t class_id);

const char* race_get_name(uint8_t race_id);

const char* rarity_get_name(ItemRarity rarity);

const char* slot_get_name(EquipSlotType slot);

#endif // ITEM_DEFINITIONS_H
