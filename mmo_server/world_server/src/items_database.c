/**
 * @file
 * Load item definitions and provide world-server item metadata lookups.
 */

#include "items_database.h"
#include "log.h"
#include "race_registry.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* Item registry: open-addressed by item id, sized to the content actually loaded.
 *
 * This was `ItemDefinition* item_table[MAX_ITEMS]` with MAX_ITEMS 100000 -- and
 * a comment calling it a hash table, which it was not. It was a direct index, so
 * it cost 800 KB resident in every world process at all times to hold a few
 * hundred items (8 MB across ten worlds), and it capped item ids at 100000 for
 * no reason except that the id was the array subscript.
 *
 * Hashing is safe here in a way it would not be for active_players or the NPC
 * pool. Those hand out interior pointers whose mutexes live inside the element,
 * so rehashing one under a held lock is a use-after-free. This registry is
 * filled once at startup, never mutated during a tick, and hands out pointers to
 * individually allocated definitions -- growth moves the bucket array, never an
 * ItemDefinition, and nothing anywhere holds a bucket.
 */
typedef struct {
    uint32_t        id;
    ItemDefinition* def;   /**< NULL marks a free bucket. */
} ItemBucket;

/** Initial bucket count; grows by doubling past a 0.7 load factor. */
#define ITEM_BUCKETS_MIN 256

static ItemBucket* g_item_buckets;
static uint32_t    g_item_bucket_count;   /**< Power of two, or 0 before first insert. */
static int         items_loaded = 0;

/** Mix an item id across the bucket space (murmur3 32-bit finalizer).
 *
 * Content ids are small and consecutive, which linear probing handles badly
 * without a mix step.
 */
static uint32_t item_hash(uint32_t id) {
    id ^= id >> 16;
    id *= 0x85ebca6bu;
    id ^= id >> 13;
    id *= 0xc2b2ae35u;
    id ^= id >> 16;
    return id;
}

/**
 * Locate the bucket holding an id, or the free bucket where it belongs.
 *
 * Terminates because the table is never allowed past a 0.7 load factor.
 *
 * @return The matching or free bucket; never NULL once the table is allocated.
 */
static ItemBucket* item_slot_for(uint32_t id) {
    const uint32_t mask = g_item_bucket_count - 1;
    uint32_t i = item_hash(id) & mask;

    while (g_item_buckets[i].def) {
        if (g_item_buckets[i].id == id) return &g_item_buckets[i];
        i = (i + 1) & mask;
    }
    return &g_item_buckets[i];
}

/**
 * Ensure the table can absorb one more insert, doubling and rehashing if not.
 *
 * @return 1 when there is room, or 0 when allocation failed.
 */
static int items_reserve_one(void) {
    if (g_item_bucket_count &&
        ((uint32_t)items_loaded + 1u) * 10u < g_item_bucket_count * 7u) {
        return 1;
    }

    const uint32_t next = g_item_bucket_count ? g_item_bucket_count * 2u
                                              : ITEM_BUCKETS_MIN;
    ItemBucket* fresh = calloc(next, sizeof(*fresh));
    if (!fresh) {
        LOG_ERROR("[ITEMS] could not grow the registry to %u buckets", next);
        return 0;
    }

    ItemBucket* old       = g_item_buckets;
    const uint32_t old_n  = g_item_bucket_count;
    g_item_buckets        = fresh;
    g_item_bucket_count   = next;

    for (uint32_t i = 0; i < old_n; i++) {
        if (old[i].def) *item_slot_for(old[i].id) = old[i];
    }
    free(old);
    return 1;
}

// Forward declarations for JSON parsing
static char* read_file(const char* filepath);
static int parse_items_json(const char* json_content);

/** Free every definition and release the bucket array. */
static void items_reset(void) {
    for (uint32_t i = 0; i < g_item_bucket_count; i++) free(g_item_buckets[i].def);
    free(g_item_buckets);
    g_item_buckets      = NULL;
    g_item_bucket_count = 0;
    items_loaded        = 0;
}

/**
 * Initialize the item registry from a JSON file.
 *
 * Replaces the current registry contents and retains definitions until items_cleanup().
 *
 * @param json_filepath  Path to the item-definition JSON file.
 * @return               1 on success, or 0 when the file cannot be read or parsed.
 */
int items_init(const char* json_filepath) {
    LOG_INFO("Loading items from: %s", json_filepath);

    // Discard any previous registry contents
    items_reset();

    // Read JSON file
    char* json_content = read_file(json_filepath);
    if (!json_content) {
        LOG_ERROR("Failed to read items file: %s", json_filepath);
        return 0;
    }

    // Parse JSON
    int result = parse_items_json(json_content);
    free(json_content);

    if (result) {
        LOG_INFO("Successfully loaded %d items", items_loaded);
    } else {
        LOG_ERROR("Failed to parse items JSON");
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
    if (!g_item_bucket_count) return NULL;
    return item_slot_for(item_id)->def;
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
    items_reset();
    LOG_INFO("Items system cleaned up");
}

/**
 * Map a character identifier to its display name.
 *
 * Race and class fuse into one identifier, so both spellings answer from the race
 * registry and there is no table of names to keep in step with the data.
 *
 * @return A registry-owned name, or "Unknown" for an unrecognized identifier.
 */
const char* class_get_name(uint8_t class_id) {
    return race_get_name(class_id);
}

/**
 * Map a character race identifier to its display name.
 *
 * @return A registry-owned name, or "Unknown" for an unrecognized identifier.
 */
const char* race_get_name(uint8_t race_id) {
    const RaceDef* race = race_get(race_id);
    return race ? race->name : "Unknown";
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

    size_t got = fread(buffer, 1, (size_t)size, f);
    buffer[got] = '\0';
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
        LOG_ERROR("No 'items' array found in JSON");
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
        /* Attribute bonuses are keyed by stat name, so an item gains a bonus to a
         * newly added stat without this loader changing at all. */
        for (int stat = 0; stat < STAT_COUNT; stat++) {
            char field[64];
            snprintf(field, sizeof(field), "bonus_%s", stat_key((StatId)stat));

            const char* val = find_json_value(obj_json, field);
            if (val) item->bonus_stats[stat] = atoi(val);
        }

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

        // Store in the registry. Any uint32_t id is addressable.
        if (!items_reserve_one()) {
            LOG_ERROR("[ITEMS] Dropped item '%s' (id %u): registry full",
                      item->name, item->id);
            free(item);
        } else {
            ItemBucket* slot = item_slot_for(item->id);

            /* A repeated id used to overwrite the earlier definition and leak
             * it, silently -- and every character already holding that item now
             * holds the new one's stats. */
            if (slot->def) {
                LOG_ERROR("[ITEMS] item id %u is defined more than once; replacing "
                          "'%s' with '%s'",
                          item->id, slot->def->name, item->name);
                free(slot->def);
                items_loaded--;
            }
            slot->id  = item->id;
            slot->def = item;
            items_loaded++;
            LOG_INFO("  Loaded: [%u] %s (%s)", item->id, item->name,
                     rarity_get_name(item->rarity));
        }

        free(obj_json);

        // Move to next item
        pos = obj_end + 1;
        while (*pos && *pos != ',' && *pos != ']') pos++;
        if (*pos == ',') pos++;
    }

    return 1;
}
