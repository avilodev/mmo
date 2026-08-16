#define _POSIX_C_SOURCE 200809L

#include "world_collision.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

// world.dat binary layout (matches client world.c):
//   Header:      3 × int32_t  (world_width, world_height, tile_size_px)
//   Tilesets:    uint8_t count; for each: uint8_t path_len, char path[], uint16_t cols, uint16_t rows
//   base_tiles:          world_width × world_height × uint16_t
//   overlay_floor:       world_width × world_height × uint16_t
//   overlay_interior:    world_width × world_height × uint16_t
//   overlay_above:       world_width × world_height × uint16_t
//   Collision:           world_width × world_height × uint8_t  (1 = solid, 0 = open)

static uint8_t* g_collision = NULL;   // flat [world_height * world_width] array
static int      g_width     = 0;
static int      g_height    = 0;
static float    g_tile_size = 16.0f;  // pixels per tile
static int      g_loaded    = 0;

int world_collision_init(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "[COLLISION] Cannot open world.dat at '%s' — collision disabled\n", path);
        return 0;
    }

    int32_t w = 0, h = 0, ts = 0;
    if (fread(&w,  sizeof(int32_t), 1, f) != 1 ||
        fread(&h,  sizeof(int32_t), 1, f) != 1 ||
        fread(&ts, sizeof(int32_t), 1, f) != 1 ||
        w <= 0 || h <= 0) {
        fprintf(stderr, "[COLLISION] world.dat header invalid\n");
        fclose(f);
        return 0;
    }

    g_tile_size = (ts > 0) ? (float)ts : 16.0f;

    // Skip tileset table (variable length)
    uint8_t ts_count = 0;
    if (fread(&ts_count, 1, 1, f) != 1) {
        fprintf(stderr, "[COLLISION] Failed to read tileset count\n");
        fclose(f);
        return 0;
    }
    for (int i = 0; i < ts_count; i++) {
        uint8_t path_len = 0;
        if (fread(&path_len, 1, 1, f) != 1) {
            fprintf(stderr, "[COLLISION] Failed to read tileset path_len\n");
            fclose(f);
            return 0;
        }
        // skip path + cols (uint16) + rows (uint16)
        if (fseek(f, path_len + 4, SEEK_CUR) != 0) {
            fprintf(stderr, "[COLLISION] Failed to seek past tileset entry\n");
            fclose(f);
            return 0;
        }
    }

    // Skip 4 tile layers: base, overlay_floor, overlay_interior, overlay_above
    long tile_bytes = (long)w * h * (long)sizeof(uint16_t) * 4;
    if (fseek(f, tile_bytes, SEEK_CUR) != 0) {
        fprintf(stderr, "[COLLISION] Failed to seek past tile data\n");
        fclose(f);
        return 0;
    }

    // Read collision layer
    size_t total = (size_t)w * (size_t)h;
    g_collision = malloc(total);
    if (!g_collision) {
        fprintf(stderr, "[COLLISION] Out of memory (%zu bytes)\n", total);
        fclose(f);
        return 0;
    }

    size_t read = fread(g_collision, 1, total, f);
    fclose(f);

    if (read != total) {
        fprintf(stderr, "[COLLISION] Short read: expected %zu bytes, got %zu\n", total, read);
        free(g_collision);
        g_collision = NULL;
        return 0;
    }

    g_width  = w;
    g_height = h;
    g_loaded = 1;

    printf("[COLLISION] Loaded %dx%d tile map (tile=%gpx) from %s\n",
           g_width, g_height, g_tile_size, path);
    return 1;
}

int world_collision_is_loaded(void) {
    return g_loaded;
}

void world_collision_extent(float* out_width, float* out_height) {
    if (out_width)  *out_width  = g_loaded ? (float)g_width  * g_tile_size : 0.0f;
    if (out_height) *out_height = g_loaded ? (float)g_height * g_tile_size : 0.0f;
}

int world_coord_is_valid(float x, float y) {
    if (!g_loaded) return 0;
    if (!isfinite(x) || !isfinite(y)) return 0;

    float w, h;
    world_collision_extent(&w, &h);
    return x >= 0.0f && x < w && y >= 0.0f && y < h;
}

int world_collision_check(float x, float y) {
    if (!g_loaded) return 1;  // No collision data — nothing is walkable

    // Converting a non-finite or out-of-range float to int is undefined
    // behaviour, so both tests happen in float, before the cast. The
    // comparisons are written to reject rather than accept NaN.
    if (!isfinite(x) || !isfinite(y)) return 1;

    float tx = x / g_tile_size;
    float ty = y / g_tile_size;

    if (!(tx >= 0.0f) || !(ty >= 0.0f) ||
        !(tx < (float)g_width) || !(ty < (float)g_height))
        return 1;  // Out of bounds = solid

    return g_collision[(int)ty * g_width + (int)tx];
}

int world_collision_check_box(float x, float y, float half_size) {
    return world_collision_check(x - half_size, y - half_size) ||
           world_collision_check(x + half_size, y - half_size) ||
           world_collision_check(x - half_size, y + half_size) ||
           world_collision_check(x + half_size, y + half_size);
}

// Upper bound on samples for one segment. A move that would need more than
// this is longer than the movement budget can legitimately produce, so it is
// refused rather than sampled coarsely.
#define COLLISION_PATH_MAX_STEPS 256

int world_collision_check_box_path(float x0, float y0,
                                   float x1, float y1,
                                   float half_size) {
    if (!g_loaded) return 1;
    if (!isfinite(x0) || !isfinite(y0) || !isfinite(x1) || !isfinite(y1))
        return 1;

    float dx = x1 - x0;
    float dy = y1 - y0;
    float distance = sqrtf(dx * dx + dy * dy);

    // Half a tile per sample: the box can never clear a solid tile by stepping
    // over it, because no tile is narrower than the sampling interval.
    float interval = g_tile_size * 0.5f;
    int steps = (int)(distance / interval) + 1;
    if (steps > COLLISION_PATH_MAX_STEPS) return 1;

    for (int i = 1; i <= steps; i++) {
        float t = (float)i / (float)steps;
        if (world_collision_check_box(x0 + dx * t, y0 + dy * t, half_size))
            return 1;
    }
    return 0;
}

void world_collision_shutdown(void) {
    free(g_collision);
    g_collision = NULL;
    g_loaded = 0;
}
