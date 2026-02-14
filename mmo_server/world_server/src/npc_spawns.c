#include "npc_spawns.h"
#include "combat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

// ---------------------------------------------------------------------------
// Minimal JSON helpers (same pattern as items_database.c)
// ---------------------------------------------------------------------------

static char* read_file(const char* filepath) {
    FILE* f = fopen(filepath, "rb");
    if (!f) return NULL;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    char* buffer = malloc(size + 1);
    if (!buffer) { fclose(f); return NULL; }

    fread(buffer, 1, size, f);
    buffer[size] = '\0';
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

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

int npc_spawns_load(const char* json_filepath, NPCWorld* world) {
    printf("Loading NPC spawns from: %s\n", json_filepath);

    char* json = read_file(json_filepath);
    if (!json) {
        fprintf(stderr, "Failed to read spawns file: %s\n", json_filepath);
        return -1;
    }

    // Find the "spawns" array
    const char* arr_start = strstr(json, "\"spawns\"");
    if (!arr_start) {
        fprintf(stderr, "No 'spawns' array found in JSON\n");
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
        int gold = json_get_int(obj, "gold_reward", 0);
        int defense = json_get_int(obj, "defense", 0);
        int evasion = json_get_int(obj, "evasion", 0);

        uint32_t npc_id = combat_npc_spawn(world, name, x, y, health,
                                            hitbox, dialogue_id, interactable,
                                            npc_type_id, respawn, category);

        if (npc_id > 0) {
            // Set optional fields that combat_npc_spawn doesn't cover
            pthread_mutex_lock(&world->lock);
            NPCEntity* npc = combat_npc_find(world, npc_id);
            if (npc) {
                if (xp >= 0) npc->xp_reward = (uint32_t)xp;
                if (gold > 0) npc->gold_reward = (uint32_t)gold;
                npc->defense = defense;
                npc->evasion = evasion;
            }
            pthread_mutex_unlock(&world->lock);
            spawned++;
        }

        free(obj);
        pos = obj_end + 1;
        while (*pos && *pos != ',' && *pos != ']') pos++;
        if (*pos == ',') pos++;
    }

    free(json);
    printf("Spawned %d NPCs from %s\n", spawned, json_filepath);
    return spawned;
}
