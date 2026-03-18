#define _POSIX_C_SOURCE 200809L

#include "world_collision.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

// world.dat binary layout (matches client world.c):
//   Header:    3 × int32_t  (world_width, world_height, tile_size_px)
//   Tile data: world_width × world_height × uint16_t
//   Collision: world_width × world_height × uint8_t   (1 = solid, 0 = open)

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

    // Skip tile data
    long tile_bytes = (long)w * h * (long)sizeof(uint16_t);
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

int world_collision_check(float x, float y) {
    if (!g_loaded) return 0;  // No collision data — allow movement

    int tx = (int)(x / g_tile_size);
    int ty = (int)(y / g_tile_size);

    if (tx < 0 || tx >= g_width || ty < 0 || ty >= g_height)
        return 1;  // Out of bounds = solid

    return g_collision[ty * g_width + tx];
}

int world_collision_check_box(float x, float y, float half_size) {
    return world_collision_check(x - half_size, y - half_size) ||
           world_collision_check(x + half_size, y - half_size) ||
           world_collision_check(x - half_size, y + half_size) ||
           world_collision_check(x + half_size, y + half_size);
}

void world_collision_shutdown(void) {
    free(g_collision);
    g_collision = NULL;
    g_loaded = 0;
}
