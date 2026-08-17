/**
 * @file
 * Load item definitions and provide world-server item metadata lookups.
 */

#include "items_database.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

// Hash table for O(1) item lookup
static ItemDefinition* item_table[MAX_ITEMS];
static int items_loaded = 0;

// Forward declarations for JSON parsing
static char* read_file(const char* filepath);
static int parse_items_json(const char* json_content);

/**
 * Initialize the item registry from a JSON file.
 *
 * Replaces the current registry contents and retains definitions until items_cleanup().
 *
 * @param json_filepath  Path to the item-definition JSON file.
 * @return               1 on success, or 0 when the file cannot be read or parsed.
 */
int items_init(const char* json_filepath) {
    printf("Loading items from: %s\n", json_filepath);
    
    // Clear the item table
    memset(item_table, 0, sizeof(item_table));
    items_loaded = 0;
    
    // Read JSON file
    char* json_content = read_file(json_filepath);
    if (!json_content) {
        fprintf(stderr, "Failed to read items file: %s\n", json_filepath);
        return 0;
    }
    
    // Parse JSON
    int result = parse_items_json(json_content);
    free(json_content);
    
    if (result) {
        printf("Successfully loaded %d items\n", items_loaded);
    } else {
        fprintf(stderr, "Failed to parse items JSON\n");
    }
    
    return result;
}

/**
 * Retrieve an item definition by identifier.
 *
 * The returned pointer remains owned by the item registry.
 *
 * @return The item definition, or NULL when the identifier is absent or out of range.
 */
const ItemDefinition* item_get(uint32_t item_id) {
    if (item_id >= MAX_ITEMS) return NULL;
    return item_table[item_id];
}

/**
 * Determine whether an item identifier is registered.
 *
 * @return 1 when registered, or 0 otherwise.
 */
int item_exists(uint32_t item_id) {
    return item_get(item_id) != NULL;
}

/**
 * Check an item's level, class, and race requirements for a character.
 *
 * @param item_id          Identifier of the item to validate.
 * @param character_level  Current character level.
 * @param character_class  CharacterClass identifier.
 * @param character_race   CharacterRace identifier.
 * @return                 1 when all requirements pass, or 0 otherwise.
 */
int item_can_equip(uint32_t item_id, uint8_t character_level, 
                   uint8_t character_class, uint8_t character_race) {
    const ItemDefinition* item = item_get(item_id);
    if (!item) return 0;
    
    // Check level requirement
    if (character_level < item->level_req) {
        return 0;
    }
    
    // Check class requirement
    if (item->class_req[0] != 0) {
        int class_valid = 0;
        for (int i = 0; i < 5 && item->class_req[i] != 0; i++) {
            if (item->class_req[i] == character_class) {
                class_valid = 1;
                break;
            }
        }
        if (!class_valid) return 0;
    }
    
    // Check race requirement
    if (item->race_req[0] != 0) {
        int race_valid = 0;
        for (int i = 0; i < 4 && item->race_req[i] != 0; i++) {
            if (item->race_req[i] == character_race) {
                race_valid = 1;
                break;
            }
        }
        if (!race_valid) return 0;
    }
    
    return 1;
}

/**
 * Report the number of registered item definitions.
 *
 * @return The number of loaded items.
 */
int items_get_count(void) {
    return items_loaded;
}

/**
 * Release all item definitions and clear the registry.
 */
void items_cleanup(void) {
    for (int i = 0; i < MAX_ITEMS; i++) {
        if (item_table[i]) {
            free(item_table[i]);
            item_table[i] = NULL;
        }
    }
    items_loaded = 0;
    printf("Items system cleaned up\n");
}

/**
 * Map a character class identifier to its display name.
 *
 * @return A static class name, or "Unknown" for an unrecognized identifier.
 */
const char* class_get_name(uint8_t class_id) {
    switch (class_id) {
        case CLASS_GLADIATOR: return "Gladiator";
        case CLASS_NINJA: return "Ninja";
        case CLASS_LANDWEAVER: return "Landweaver";
        case CLASS_SPIRIT: return "Spirit";
        default: return "Unknown";
    }
}

/**
 * Map a character race identifier to its display name.
 *
 * @return A static race name, or "Unknown" for an unrecognized identifier.
 */
const char* race_get_name(uint8_t race_id) {
    switch (race_id) {
        case RACE_HUMAN: return "Human";
        case RACE_PYSECK: return "Pyseck";
        case RACE_INFOR: return "Infor";
        default: return "Unknown";
    }
}

/**
 * Map an item rarity to its display name.
 *
 * @return A static rarity name, or "Unknown" for an unrecognized value.
 */
const char* rarity_get_name(ItemRarity rarity) {
    switch (rarity) {
        case RARITY_COMMON: return "Common";
        case RARITY_UNCOMMON: return "Uncommon";
        case RARITY_RARE: return "Rare";
        case RARITY_EPIC: return "Epic";
        case RARITY_LEGENDARY: return "Legendary";
        default: return "Unknown";
    }
}

/**
 * Map an equipment slot to its display name.
 *
 * @return A static slot name, or "None" for an unrecognized value.
 */
const char* slot_get_name(EquipSlotType slot) {
    switch (slot) {
        case SLOT_HELMET: return "Helmet";
        case SLOT_GLOVES: return "Gloves";
        case SLOT_CHEST: return "Chest";
        case SLOT_LEGGINGS: return "Leggings";
        case SLOT_BOOTS: return "Boots";
        case SLOT_MAIN_HAND: return "Main Hand";
        case SLOT_OFF_HAND: return "Off Hand";
        case SLOT_TWO_HANDED: return "Two-Handed";
        default: return "None";
    }
}

/**
 * Read an entire file into a terminated buffer.
 *
 * The caller must free the returned buffer.
 *
 * @param filepath  Path to the file to read.
 * @return          An allocated buffer, or NULL when opening or allocation fails.
 */
static char* read_file(const char* filepath) {
    FILE* f = fopen(filepath, "rb");
    if (!f) return NULL;
    
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    char* buffer = malloc(size + 1);
    if (!buffer) {
        fclose(f);
        return NULL;
    }
    
    fread(buffer, 1, size, f);
    buffer[size] = '\0';
    fclose(f);
    
    return buffer;
}

static EquipSlotType parse_slot(const char* slot_str) {
    if (strcmp(slot_str, "helmet") == 0) return SLOT_HELMET;
    if (strcmp(slot_str, "gloves") == 0) return SLOT_GLOVES;
    if (strcmp(slot_str, "chest") == 0) return SLOT_CHEST;
    if (strcmp(slot_str, "leggings") == 0) return SLOT_LEGGINGS;
    if (strcmp(slot_str, "boots") == 0) return SLOT_BOOTS;
    if (strcmp(slot_str, "main_hand") == 0) return SLOT_MAIN_HAND;
    if (strcmp(slot_str, "off_hand") == 0) return SLOT_OFF_HAND;
    if (strcmp(slot_str, "two_handed") == 0) return SLOT_TWO_HANDED;
    return SLOT_NONE;
}

static ItemType parse_type(const char* type_str) {
    if (strcmp(type_str, "item") == 0) return ITEM_TYPE_ITEM;
    if (strcmp(type_str, "weapon") == 0) return ITEM_TYPE_WEAPON;
    if (strcmp(type_str, "shield") == 0) return ITEM_TYPE_SHIELD;
    if (strcmp(type_str, "armor") == 0) return ITEM_TYPE_ARMOR;
    if (strcmp(type_str, "consumable") == 0) return ITEM_TYPE_CONSUMABLE;
    if (strcmp(type_str, "quest") == 0) return ITEM_TYPE_QUEST;
    return ITEM_TYPE_ITEM;
}

static UseEffectType parse_use_effect(const char* effect_str) {
    if (strcmp(effect_str, "restore_health") == 0) return USE_EFFECT_RESTORE_HEALTH;
    if (strcmp(effect_str, "restore_mana") == 0) return USE_EFFECT_RESTORE_MANA;
    if (strcmp(effect_str, "restore_both") == 0) return USE_EFFECT_RESTORE_BOTH;
    return USE_EFFECT_NONE;
}

static ItemRarity parse_rarity(const char* rarity_str) {
    if (strcmp(rarity_str, "common") == 0) return RARITY_COMMON;
    if (strcmp(rarity_str, "uncommon") == 0) return RARITY_UNCOMMON;
    if (strcmp(rarity_str, "rare") == 0) return RARITY_RARE;
    if (strcmp(rarity_str, "epic") == 0) return RARITY_EPIC;
    if (strcmp(rarity_str, "legendary") == 0) return RARITY_LEGENDARY;
    return RARITY_COMMON;
}

/** Locate the first value text following a named JSON key.
 *
 * @return A pointer into json after leading whitespace, or NULL when the key or colon is absent.
 */
static const char* find_json_value(const char* json, const char* key) {
    char search[128];
    snprintf(search, sizeof(search), "\"%s\"", key);
    
    const char* pos = strstr(json, search);
    if (!pos) return NULL;
    
    // Skip past the key and find the colon
    pos = strchr(pos, ':');
    if (!pos) return NULL;
    pos++;
    
    // Skip whitespace
    while (*pos && isspace(*pos)) pos++;
    
    return pos;
}

/**
 * Parse item objects into the global registry.
 *
 * @param json_content  Terminated JSON document containing an items array.
 * @return              1 after scanning the array, or 0 when the array is absent or malformed at its start.
 */
static int parse_items_json(const char* json_content) {
    // Find the "items" array
    const char* items_start = strstr(json_content, "\"items\"");
    if (!items_start) {
        fprintf(stderr, "No 'items' array found in JSON\n");
        return 0;
    }
    
    // Find the opening bracket of the array
    const char* array_start = strchr(items_start, '[');
    if (!array_start) return 0;
    
    const char* pos = array_start + 1;
    
    // Parse each item object
    while (*pos) {
        // Skip whitespace
        while (*pos && isspace(*pos)) pos++;
        
        if (*pos == ']') break; // End of array
        if (*pos != '{') {
            pos++;
            continue;
        }
        
        // Find the end of this object
        const char* obj_start = pos;
        int brace_count = 0;
        const char* obj_end = pos;
        
        while (*obj_end) {
            if (*obj_end == '{') brace_count++;
            if (*obj_end == '}') {
                brace_count--;
                if (brace_count == 0) break;
            }
            obj_end++;
        }
        
        if (brace_count != 0) break; // Malformed JSON
        
        // Extract this object
        int obj_len = obj_end - obj_start + 1;
        char* obj_json = malloc(obj_len + 1);
        memcpy(obj_json, obj_start, obj_len);
        obj_json[obj_len] = '\0';
        
        // Parse the item
        ItemDefinition* item = calloc(1, sizeof(ItemDefinition));
        
        // Parse ID
        const char* id_val = find_json_value(obj_json, "id");
        if (id_val) item->id = atoi(id_val);
        
        // Parse name
        const char* name_val = find_json_value(obj_json, "name");
        if (name_val && *name_val == '"') {
            const char* name_end = strchr(name_val + 1, '"');
            if (name_end) {
                int name_len = name_end - (name_val + 1);
                if (name_len >= MAX_ITEM_NAME) name_len = MAX_ITEM_NAME - 1;
                memcpy(item->name, name_val + 1, name_len);
                item->name[name_len] = '\0';
            }
        }
        
        // Parse type
        const char* type_val = find_json_value(obj_json, "type");
        if (type_val && *type_val == '"') {
            const char* type_end = strchr(type_val + 1, '"');
            if (type_end) {
                int type_len = type_end - (type_val + 1);
                char type_str[32] = {0};
                if (type_len < 32) {
                    memcpy(type_str, type_val + 1, type_len);
                    item->type = parse_type(type_str);
                }
            }
        }
        
        // Parse slot
        const char* slot_val = find_json_value(obj_json, "slot");
        if (slot_val && *slot_val == '"') {
            const char* slot_end = strchr(slot_val + 1, '"');
            if (slot_end) {
                int slot_len = slot_end - (slot_val + 1);
                char slot_str[32] = {0};
                if (slot_len < 32) {
                    memcpy(slot_str, slot_val + 1, slot_len);
                    item->slot = parse_slot(slot_str);
                    item->is_two_handed = (item->slot == SLOT_TWO_HANDED) ? 1 : 0;
                }
            }
        }
        
        // Parse damage
        const char* dmg_val = find_json_value(obj_json, "damage");
        if (dmg_val) item->damage = atoi(dmg_val);
        
        // Parse defense
        const char* def_val = find_json_value(obj_json, "defense");
        if (def_val) item->defense = atoi(def_val);
        
        // Parse level requirement
        const char* lvl_val = find_json_value(obj_json, "level_req");
        if (lvl_val) item->level_req = atoi(lvl_val);
        
        // Parse class requirements
        const char* class_val = find_json_value(obj_json, "class_req");
        if (class_val && *class_val == '[') {
            const char* arr_pos = class_val + 1;
            int class_idx = 0;
            while (*arr_pos && *arr_pos != ']' && class_idx < 5) {
                while (*arr_pos && !isdigit(*arr_pos) && *arr_pos != ']') arr_pos++;
                if (*arr_pos == ']') break;
                if (isdigit(*arr_pos)) {
                    item->class_req[class_idx++] = atoi(arr_pos);
                    while (*arr_pos && isdigit(*arr_pos)) arr_pos++;
                }
            }
        }
        
        // Parse rarity
        const char* rarity_val = find_json_value(obj_json, "rarity");
        if (rarity_val && *rarity_val == '"') {
            const char* rarity_end = strchr(rarity_val + 1, '"');
            if (rarity_end) {
                int rarity_len = rarity_end - (rarity_val + 1);
                char rarity_str[32] = {0};
                if (rarity_len < 32) {
                    memcpy(rarity_str, rarity_val + 1, rarity_len);
                    item->rarity = parse_rarity(rarity_str);
                }
            }
        }
        
        // Parse consumable effect
        const char* effect_val = find_json_value(obj_json, "use_effect");
        if (effect_val && *effect_val == '"') {
            const char* effect_end = strchr(effect_val + 1, '"');
            if (effect_end) {
                int effect_len = effect_end - (effect_val + 1);
                char effect_str[32] = {0};
                if (effect_len < 32) {
                    memcpy(effect_str, effect_val + 1, effect_len);
                    item->use_effect = parse_use_effect(effect_str);
                }
            }
        }

        // Parse consumable value
        const char* use_val = find_json_value(obj_json, "use_value");
        if (use_val) item->use_value = atoi(use_val);

        // Parse consumable cooldown
        const char* cd_val = find_json_value(obj_json, "use_cooldown");
        if (cd_val) item->use_cooldown = (float)atof(cd_val);

        // Parse stat bonuses
        const char* bs_val = find_json_value(obj_json, "bonus_strength");
        if (bs_val) item->bonus_strength = atoi(bs_val);
        const char* ba_val = find_json_value(obj_json, "bonus_agility");
        if (ba_val) item->bonus_agility = atoi(ba_val);
        const char* bi_val = find_json_value(obj_json, "bonus_intelligence");
        if (bi_val) item->bonus_intelligence = atoi(bi_val);
        const char* bw_val = find_json_value(obj_json, "bonus_wisdom");
        if (bw_val) item->bonus_wisdom = atoi(bw_val);
        const char* bd_val = find_json_value(obj_json, "bonus_defense");
        if (bd_val) item->bonus_defense = atoi(bd_val);
        const char* be_val = find_json_value(obj_json, "bonus_evasion");
        if (be_val) item->bonus_evasion = atoi(be_val);
        const char* bv_val = find_json_value(obj_json, "bonus_vitality");
        if (bv_val) item->bonus_vitality = atoi(bv_val);
        const char* bl_val = find_json_value(obj_json, "bonus_luck");
        if (bl_val) item->bonus_luck = atoi(bl_val);

        // default absent or zero stack limits to one
        const char* stack_val = find_json_value(obj_json, "max_stack");
        item->max_stack = stack_val ? (uint16_t)atoi(stack_val) : 1;
        if (item->max_stack == 0) item->max_stack = 1;
        item->stackable = (item->max_stack > 1) ? 1 : 0;

        // accept JSON booleans and numeric flags
        const char* bop_val = find_json_value(obj_json, "bind_on_pickup");
        item->bind_on_pickup = (bop_val && (*bop_val == 't' || *bop_val == '1')) ? 1 : 0;
        const char* boe_val = find_json_value(obj_json, "bind_on_equip");
        item->bind_on_equip = (boe_val && (*boe_val == 't' || *boe_val == '1')) ? 1 : 0;

        // derive absent values from rarity
        const char* value_val = find_json_value(obj_json, "value");
        item->value = value_val ? (uint32_t)atoi(value_val)
                                : (uint32_t)((item->rarity + 1) * 10);

        // Store in hash table
        if (item->id < MAX_ITEMS) {
            item_table[item->id] = item;
            items_loaded++;
            printf("  Loaded: [%u] %s (%s)\n", item->id, item->name, 
                   rarity_get_name(item->rarity));
        } else {
            free(item);
        }
        
        free(obj_json);
        
        // Move to next item
        pos = obj_end + 1;
        while (*pos && *pos != ',' && *pos != ']') pos++;
        if (*pos == ',') pos++;
    }
    
    return 1;
}
