/**
 * @file
 * Load and look up the baked tile heights.
 *
 * Layout, little-endian, written by export_city_collision.py:
 *
 *   char   magic[4]   "CHGT"
 *   uint32 version
 *   int32  min_x      first column, in tiles from the courtyard centre
 *   int32  min_y      first row, in tiles from the courtyard centre
 *   int32  width      columns
 *   int32  height     rows
 *   uint8  tops[height][width]   metres / HEIGHT_UNIT_M; 0 where not solid
 */
#include "world/city_heights.h"
#include "core/client_log.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FILE_MAGIC     "CHGT"
#define FILE_VERSION   1u
#define HEIGHT_UNIT_M  0.25f
#define MAX_GRID_SIDE  20000

static uint8_t* g_tops;
static int      g_width, g_height, g_min_x, g_min_y;

static int read_i32(FILE* f, int32_t* out) {
    uint8_t b[4];
    if (fread(b, 1, 4, f) != 4) return 0;
    *out = (int32_t)((uint32_t)b[0] | ((uint32_t)b[1] << 8) |
                     ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24));
    return 1;
}

void city_heights_free(void) {
    free(g_tops);
    g_tops   = NULL;
    g_width  = 0;
    g_height = 0;
}

int city_heights_load(const char* path) {
    city_heights_free();

    FILE* f = fopen(path, "rb");
    if (!f) {
        CLOG_ERROR("[CITY] Cannot open %s", path);
        return 0;
    }

    char    magic[4];
    int32_t version = 0, min_x = 0, min_y = 0, width = 0, height = 0;
    int ok = fread(magic, 1, 4, f) == 4 && memcmp(magic, FILE_MAGIC, 4) == 0 &&
             read_i32(f, &version) && (uint32_t)version == FILE_VERSION &&
             read_i32(f, &min_x) && read_i32(f, &min_y) &&
             read_i32(f, &width) && read_i32(f, &height) &&
             width > 0 && height > 0 && width <= MAX_GRID_SIDE && height <= MAX_GRID_SIDE;
    if (ok) {
        size_t n = (size_t)width * (size_t)height;
        g_tops = malloc(n);
        ok = g_tops && fread(g_tops, 1, n, f) == n;
        g_width = width; g_height = height; g_min_x = min_x; g_min_y = min_y;
    }
    fclose(f);

    if (!ok) {
        CLOG_ERROR("[CITY] %s is not a valid city heights file", path);
        city_heights_free();
    }
    return ok;
}

float city_heights_at(void* ctx, float world_x, float world_y) {
    (void)ctx;
    if (!g_tops) return 0.0f;

    int col = (int)floorf(world_x / WORLD_TILE_PX) - CITY_ENNARA_TILE_X - g_min_x;
    int row = (int)floorf(world_y / WORLD_TILE_PX) - CITY_ENNARA_TILE_Y - g_min_y;
    if (col < 0 || col >= g_width || row < 0 || row >= g_height) return 0.0f;
    return (float)g_tops[(size_t)row * g_width + col] * HEIGHT_UNIT_M * CITY_UNITS_PER_METRE;
}
