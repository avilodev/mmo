#ifndef ITEM_DEFINITIONS_H
#define ITEM_DEFINITIONS_H

#include <stdint.h>

// Maximum items the system can handle
#define MAX_ITEMS 100000
#define MAX_ITEM_NAME 64

// Item types
typedef enum {
    ITEM_TYPE_ITEM = 0,        // Generic items (crafting materials, etc)
    ITEM_TYPE_WEAPON = 1,      // Weapons
    ITEM_TYPE_SHIELD = 2,      // Shields
    ITEM_TYPE_ARMOR = 3,       // Armor pieces
    ITEM_TYPE_CONSUMABLE = 4,  // Potions, food, etc
    ITEM_TYPE_QUEST = 5        // Quest items
} ItemType;

// Equipment slots
typedef enum {
    SLOT_NONE = 0,
    SLOT_HELMET = 1,
    SLOT_GLOVES = 2,
    SLOT_CHEST = 3,
    SLOT_LEGGINGS = 4,
    SLOT_BOOTS = 5,
    SLOT_MAIN_HAND = 6,
    SLOT_OFF_HAND = 7,         // Shield or second weapon
    SLOT_TWO_HANDED = 8        // Two-handed weapons
} EquipSlot;

// Consumable effect types
typedef enum {
    USE_EFFECT_NONE = 0,
    USE_EFFECT_RESTORE_HEALTH = 1,
    USE_EFFECT_RESTORE_MANA = 2,
    USE_EFFECT_RESTORE_BOTH = 3,
} UseEffectType;

// Item rarity
typedef enum {
    RARITY_COMMON = 0,
    RARITY_UNCOMMON = 1,
    RARITY_RARE = 2,
    RARITY_EPIC = 3,
    RARITY_LEGENDARY = 4
} ItemRarity;

// Class definitions
typedef enum {
    CLASS_GLADIATOR = 1,   // Tank - Heavy Armor
    CLASS_NINJA = 2,       // Damage - Light Armor
    CLASS_LANDWEAVER = 3,  // Support - Medium Armor
    CLASS_SPIRIT = 4       // Healer - Light Armor
} CharacterClass;

// Race definitions
typedef enum {
    RACE_HUMAN = 1,
    RACE_PYSECK = 2,   // Small
    RACE_INFOR = 3     // Infernal
} CharacterRace;

// Item definition structure
typedef struct {
    uint32_t id;
    char name[MAX_ITEM_NAME];
    
    // Type and slot
    ItemType type;
    EquipSlot slot;
    
    // Stats
    uint32_t damage;           // For weapons
    uint32_t defense;          // For armor/shields

    // Stat bonuses (applied when equipped)
    int32_t bonus_strength;
    int32_t bonus_agility;
    int32_t bonus_intelligence;
    int32_t bonus_wisdom;
    int32_t bonus_defense;
    int32_t bonus_evasion;
    int32_t bonus_vitality;
    int32_t bonus_luck;
    
    // Requirements
    uint8_t level_req;         // Minimum level required
    uint8_t class_req[5];      // Required classes (0 = any, otherwise specific class IDs)
    uint8_t race_req[4];       // Required races (0 = any)
    
    // Properties
    ItemRarity rarity;
    uint8_t craftable;         // 0 = no, 1 = yes
    uint8_t stackable;         // 0 = no, 1 = yes
    uint16_t max_stack;        // Maximum stack size
    uint32_t value;            // Gold value (for selling)
    
    // Consumable
    uint8_t use_effect;        // UseEffectType — what happens when used
    int32_t use_value;         // How much to restore/apply
    float   use_cooldown;      // Seconds before this consumable can be used again

    // Flags
    uint8_t is_two_handed;     // 1 if two-handed weapon
    uint8_t bind_on_pickup;    // 1 if binds when picked up
    uint8_t bind_on_equip;     // 1 if binds when equipped
} ItemDefinition;

// Initialize the item system from JSON file
int items_init(const char* json_filepath);

// Get item definition by ID (O(1) lookup)
const ItemDefinition* item_get(uint32_t item_id);

// Check if item exists
int item_exists(uint32_t item_id);

// Validate if a character can equip this item
int item_can_equip(uint32_t item_id, uint8_t character_level, 
                   uint8_t character_class, uint8_t character_race);

// Get total number of loaded items
int items_get_count(void);

// Cleanup item system
void items_cleanup(void);

// Helper: Get class name
const char* class_get_name(uint8_t class_id);

// Helper: Get race name
const char* race_get_name(uint8_t race_id);

// Helper: Get rarity name
const char* rarity_get_name(ItemRarity rarity);

// Helper: Get slot name
const char* slot_get_name(EquipSlot slot);

#endif // ITEM_DEFINITIONS_H