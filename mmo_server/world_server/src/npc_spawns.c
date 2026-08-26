/**
 * @file
 * Load configured NPC spawn records into the world-server NPC pool.
 */

#include "npc_spawns.h"
#include "npc_world.h"
#include "combat.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

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

    char* buffer = malloc(size + 1);
    if (!buffer) { fclose(f); return NULL; }

    size_t got = fread(buffer, 1, (size_t)size, f);
    buffer[got] = '\0';
    fclose(f);
    return buffer;
}

static const char* find_json_value(const char* json, const char* key) {
    char search[128];
    snprintf(search, sizeof(search), "\"%s\"", key);

    const char* pos = strstr(json, search);
    if (!pos) return NULL;

    pos = strchr(pos, ':');
    if (!pos) return NULL;
    pos++;

    while (*pos && isspace(*pos)) pos++;
    return pos;
}

static int json_get_int(const char* json, const char* key, int fallback) {
    const char* val = find_json_value(json, key);
    if (!val) return fallback;
    return atoi(val);
}

static float json_get_float(const char* json, const char* key, float fallback) {
    const char* val = find_json_value(json, key);
    if (!val) return fallback;
    return (float)atof(val);
}

static int json_get_string(const char* json, const char* key, char* out, int out_size) {
    const char* val = find_json_value(json, key);
    if (!val || *val != '"') return 0;

    const char* end = strchr(val + 1, '"');
    if (!end) return 0;

    int len = end - (val + 1);
    if (len >= out_size) len = out_size - 1;
    memcpy(out, val + 1, len);
    out[len] = '\0';
    return 1;
}

static uint8_t parse_category(const char* str) {
    if (strcmp(str, "hostile") == 0) return NPC_CATEGORY_HOSTILE;
    if (strcmp(str, "quest") == 0)   return NPC_CATEGORY_QUEST;
    return NPC_CATEGORY_PASSIVE;
}

/**
 * Parse NPC spawn objects and add them to an NPC world.
 *
 * @param json_filepath  Path to the spawn-definition JSON file.
 * @param world          Initialized NPC world receiving the spawns.
 * @return               The number spawned, or -1 when the file or root array is unavailable.
 */
int npc_spawns_load(const char* json_filepath, NPCWorld* world) {
    LOG_INFO("Loading NPC spawns from: %s", json_filepath);

    char* json = read_file(json_filepath);
    if (!json) {
        LOG_ERROR("Failed to read spawns file: %s", json_filepath);
        return -1;
    }

    // Find the "spawns" array
    const char* arr_start = strstr(json, "\"spawns\"");
    if (!arr_start) {
        LOG_ERROR("No 'spawns' array found in JSON");
        free(json);
        return -1;
    }

    const char* bracket = strchr(arr_start, '[');
    if (!bracket) { free(json); return -1; }

    const char* pos = bracket + 1;
    int spawned = 0;

    while (*pos) {
        while (*pos && isspace(*pos)) pos++;
        if (*pos == ']') break;
        if (*pos != '{') { pos++; continue; }

        // Find matching close brace
        const char* obj_start = pos;
        int brace = 0;
        const char* obj_end = pos;
        while (*obj_end) {
            if (*obj_end == '{') brace++;
            if (*obj_end == '}') { brace--; if (brace == 0) break; }
            obj_end++;
        }
        if (brace != 0) break;

        int obj_len = obj_end - obj_start + 1;
        char* obj = malloc(obj_len + 1);
        memcpy(obj, obj_start, obj_len);
        obj[obj_len] = '\0';

        // Parse fields
        char name[32] = {0};
        json_get_string(obj, "name", name, sizeof(name));

        char cat_str[16] = "passive";
        json_get_string(obj, "category", cat_str, sizeof(cat_str));
        uint8_t category = parse_category(cat_str);

        uint16_t npc_type_id = (uint16_t)json_get_int(obj, "npc_type_id", 0);
        float x = json_get_float(obj, "x", 0.0f);
        float y = json_get_float(obj, "y", 0.0f);
        int health = json_get_int(obj, "health", 100);
        float hitbox = json_get_float(obj, "hitbox_radius", 16.0f);
        uint32_t dialogue_id = (uint32_t)json_get_int(obj, "dialogue_id", 0);
        uint8_t interactable = (uint8_t)json_get_int(obj, "is_interactable", 0);
        float respawn = json_get_float(obj, "respawn_time", 0.0f);
        int xp = json_get_int(obj, "xp_reward", -1);
        /* "gold_reward" in older spawn data is read and discarded: kills pay
         * experience and loot only, never coin. */
        if (json_get_int(obj, "gold_reward", 0) > 0)
            LOG_DEBUG("[NPC] spawn '%s' still sets gold_reward — ignoring, "
                      "enemies drop items instead", name);
        /* "defense" is still accepted as a spelling of armor so existing spawn data
         * keeps working; "armor" is the name that matches the player attribute. */
        int armor = json_get_int(obj, "armor", json_get_int(obj, "defense", 0));

        uint32_t npc_id = npc_world_spawn(world, name, x, y, health,
                                            hitbox, dialogue_id, interactable,
                                            npc_type_id, respawn, category);

        if (npc_id > 0) {
            // Set optional fields that npc_world_spawn doesn't cover
            NPCEntity* npc = npc_world_acquire(world, npc_id);
            if (npc) {
                if (xp >= 0) npc->xp_reward = (uint32_t)xp;
                npc->armor = armor;
                npc_world_release(world, npc);
            }
            spawned++;
        }

        free(obj);
        pos = obj_end + 1;
        while (*pos && *pos != ',' && *pos != ']') pos++;
        if (*pos == ',') pos++;
    }

    free(json);
    LOG_INFO("Spawned %d NPCs from %s", spawned, json_filepath);
    return spawned;
}
