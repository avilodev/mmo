/**
 * @file
 * Load rectangular world zones and resolve positions to the most specific zone.
 */

#include "zone_system.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static WorldZone g_zones[MAX_WORLD_ZONES];
static int       g_zone_count = 0;

static const char* skip_ws(const char* p) {
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    return p;
}

static const char* parse_string(const char* p, char* out, int cap) {
    if (*p != '"') return p;
    p++;
    int i = 0;
    while (*p && *p != '"') {
        if (i < cap - 1) out[i++] = *p;
        p++;
    }
    out[i] = '\0';
    if (*p == '"') p++;
    return p;
}

static const char* parse_number(const char* p, float* out) {
    char* end;
    *out = strtof(p, &end);
    return end;
}

/**
 * Locate a key's value within the current flat JSON object.
 *
 * @return The value position, or NULL when the key is absent.
 */
static const char* find_key(const char* obj_start, const char* key) {
    const char* p = obj_start;
    while (*p && *p != '}') {
        p = skip_ws(p);
        if (*p != '"') { p++; continue; }

        char k[64] = {0};
        p = parse_string(p, k, sizeof(k));
        p = skip_ws(p);
        if (*p == ':') p++;
        p = skip_ws(p);

        if (strcmp(k, key) == 0) return p;

        // Skip value
        if (*p == '"') {
            p++;
            while (*p && *p != '"') p++;
            if (*p) p++;
        } else {
            while (*p && *p != ',' && *p != '}') p++;
        }
        if (*p == ',') p++;
    }
    return NULL;
}

/**
 * Initialize world zones from a JSON file.
 *
 * @return The number of loaded zones, or -1 when opening or allocation fails.
 */
int zone_system_init(const char* json_path) {
    FILE* f = fopen(json_path, "r");
    if (!f) {
        fprintf(stderr, "[ZONES] Cannot open %s\n", json_path);
        return -1;
    }

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);

    char* buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return -1; }

    fread(buf, 1, (size_t)sz, f);
    buf[sz] = '\0';
    fclose(f);

    g_zone_count = 0;
    const char* p = buf;

    // Walk each { ... } object inside the "zones" array
    while (*p && g_zone_count < MAX_WORLD_ZONES) {
        p = strchr(p, '{');
        if (!p) break;
        const char* obj = p;
        p++;

        // Find matching closing brace
        int depth = 1;
        const char* obj_end = p;
        while (*obj_end && depth > 0) {
            if (*obj_end == '{') depth++;
            else if (*obj_end == '}') depth--;
            obj_end++;
        }

        // Skip the outer { "zones": [ ... ] } wrapper — only take objects with "id"
        const char* id_val = find_key(obj, "id");
        if (!id_val) continue;

        WorldZone* z = &g_zones[g_zone_count];
        memset(z, 0, sizeof(*z));

        float tmp = 0.0f;
        parse_number(id_val, &tmp);
        z->id = (uint8_t)tmp;

        const char* v;

        v = find_key(obj, "type");
        if (v) { parse_number(v, &tmp); z->type = (uint8_t)tmp; }

        v = find_key(obj, "name");
        if (v && *v == '"') parse_string(v, z->name, sizeof(z->name));

        v = find_key(obj, "x"); if (v) parse_number(v, &z->x);
        v = find_key(obj, "y"); if (v) parse_number(v, &z->y);
        v = find_key(obj, "w"); if (v) parse_number(v, &z->w);
        v = find_key(obj, "h"); if (v) parse_number(v, &z->h);

        if (z->id > 0 && z->name[0] != '\0') {
            printf("[ZONES] Loaded zone %d '%s' type=%d rect=(%.0f,%.0f,%.0f,%.0f)\n",
                   z->id, z->name, z->type, z->x, z->y, z->w, z->h);
            g_zone_count++;
        }

        p = obj_end;
    }

    free(buf);
    printf("[ZONES] %d zones loaded from %s\n", g_zone_count, json_path);
    return g_zone_count;
}

/**
 * Clear the world-zone registry.
 */
void zone_system_cleanup(void) {
    g_zone_count = 0;
}

/**
 * Find the smallest registered zone containing a world position.
 *
 * The returned pointer remains owned by the zone registry.
 *
 * @param wx  World X coordinate.
 * @param wy  World Y coordinate.
 * @return    The most specific containing zone, or NULL when outside every zone.
 */
const WorldZone* zone_lookup(float wx, float wy) {
    const WorldZone* best = NULL;
    float best_area = 1e30f;

    for (int i = 0; i < g_zone_count; i++) {
        const WorldZone* z = &g_zones[i];
        if (wx >= z->x && wx < z->x + z->w &&
            wy >= z->y && wy < z->y + z->h) {
            float area = z->w * z->h;
            if (area < best_area) {
                best_area = area;
                best = z;
            }
        }
    }
    return best;
}
