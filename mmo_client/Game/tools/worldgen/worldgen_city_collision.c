/**
 * @file
 * Read the city's baked collision footprints.
 *
 * Game/tools/export_city_collision.py writes the file from the COLLISION
 * collection of ennara_city.blend. Layout, little-endian:
 *
 *   char   magic[4]   "CCOL"
 *   uint32 version
 *   int32  min_x      first column, in tiles from the courtyard centre
 *   int32  min_y      first row, in tiles from the courtyard centre
 *   int32  width      columns
 *   int32  height     rows
 *   per row, top to bottom:
 *       uint16 run_count
 *       run_count times: uint16 start (from min_x), uint16 length
 */
#include "world/worldgen.h"
#include "world_regions.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FILE_MAGIC   "CCOL"
#define FILE_VERSION 1u

/** Refuse a grid no city could have rather than allocating it. */
#define MAX_GRID_SIDE 20000

static uint8_t* g_solid;
static int      g_width;
static int      g_height;
static int      g_min_x;
static int      g_min_y;

static int read_u16(FILE* f, uint16_t* out) {
    uint8_t b[2];
    if (fread(b, 1, 2, f) != 2) return 0;
    *out = (uint16_t)(b[0] | (b[1] << 8));
    return 1;
}

static int read_i32(FILE* f, int32_t* out) {
    uint8_t b[4];
    if (fread(b, 1, 4, f) != 4) return 0;
    *out = (int32_t)((uint32_t)b[0] | ((uint32_t)b[1] << 8) |
                     ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24));
    return 1;
}

/** Decode the runs of every row into the bitmap; 0 if a run leaves the grid. */
static int read_rows(FILE* f) {
    for (int y = 0; y < g_height; y++) {
        uint16_t run_count;
        if (!read_u16(f, &run_count)) return 0;
        for (int r = 0; r < run_count; r++) {
            uint16_t start, length;
            if (!read_u16(f, &start) || !read_u16(f, &length)) return 0;
            if ((int)start + (int)length > g_width) return 0;
            memset(&g_solid[(size_t)y * g_width + start], 1, length);
        }
    }
    return 1;
}

void worldgen_city_collision_free(void) {
    free(g_solid);
    g_solid  = NULL;
    g_width  = 0;
    g_height = 0;
}

int worldgen_city_collision_load(const char* path) {
    worldgen_city_collision_free();

    FILE* f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "[WORLDGEN] Cannot open %s\n", path);
        return 0;
    }

    char    magic[4];
    int32_t version = 0, min_x = 0, min_y = 0, width = 0, height = 0;
    int     ok = fread(magic, 1, 4, f) == 4 && memcmp(magic, FILE_MAGIC, 4) == 0 &&
                 read_i32(f, &version) && (uint32_t)version == FILE_VERSION &&
                 read_i32(f, &min_x) && read_i32(f, &min_y) &&
                 read_i32(f, &width) && read_i32(f, &height) &&
                 width > 0 && height > 0 &&
                 width <= MAX_GRID_SIDE && height <= MAX_GRID_SIDE;

    if (ok) {
        g_solid = calloc((size_t)width * (size_t)height, 1);
        g_width  = width;
        g_height = height;
        g_min_x  = min_x;
        g_min_y  = min_y;
        ok = g_solid != NULL && read_rows(f);
    }
    fclose(f);

    if (!ok) {
        fprintf(stderr, "[WORLDGEN] %s is not a valid city collision file\n", path);
        worldgen_city_collision_free();
    }
    return ok;
}

int worldgen_city_collision_solid(int x, int y) {
    if (!g_solid) return 0;

    int col = x - CITY_ENNARA_TILE_X - g_min_x;
    int row = y - CITY_ENNARA_TILE_Y - g_min_y;
    if (col < 0 || col >= g_width || row < 0 || row >= g_height) return 0;
    return g_solid[(size_t)row * g_width + col];
}
