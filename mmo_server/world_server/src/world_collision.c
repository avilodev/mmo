/**
 * @file
 * Load the world.dat collision layer and answer bounded world-space collision queries.
 */

#define _POSIX_C_SOURCE 200809L

#include "world_collision.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

// collision bytes follow four uint16 tile layers

static uint8_t* g_collision = NULL;   // flat [world_height * world_width] array
static int      g_width     = 0;
static int      g_height    = 0;
static float    g_tile_size = 16.0f;  // pixels per tile
static int      g_loaded    = 0;

/**
 * Load the collision layer from a client-format world.dat file.
 *
 * Reads native-width integer fields and treats short or malformed input as failure.
 *
 * @param path  Path to the binary world file.
 * @return      1 on success, or 0 on I/O, format, or allocation failure.
 */
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

/**
 * Determine whether collision data is loaded.
 *
 * @return 1 when loaded, or 0 otherwise.
 */
int world_collision_is_loaded(void) {
    return g_loaded;
}

/**
 * Report the loaded world's pixel extent.
 *
 * @param out_width   Receives width in pixels; may be NULL.
 * @param out_height  Receives height in pixels; may be NULL.
 */
void world_collision_extent(float* out_width, float* out_height) {
    if (out_width)  *out_width  = g_loaded ? (float)g_width  * g_tile_size : 0.0f;
    if (out_height) *out_height = g_loaded ? (float)g_height * g_tile_size : 0.0f;
}

/**
 * Check that a finite position lies within the loaded world extent.
 *
 * @return 1 when valid, or 0 otherwise.
 */
int world_coord_is_valid(float x, float y) {
    if (!g_loaded) return 0;
    if (!isfinite(x) || !isfinite(y)) return 0;

    float w, h;
    world_collision_extent(&w, &h);
    return x >= 0.0f && x < w && y >= 0.0f && y < h;
}

/**
 * Test a world position against the collision tile layer.
 *
 * Unloaded maps, non-finite coordinates, and out-of-bounds positions fail closed.
 *
 * @return 1 for solid or invalid space, or 0 for open space.
 */
int world_collision_check(float x, float y) {
    if (!g_loaded) return 1;  // No collision data — nothing is walkable

    // validate floats before integer conversion
    if (!isfinite(x) || !isfinite(y)) return 1;

    float tx = x / g_tile_size;
    float ty = y / g_tile_size;

    if (!(tx >= 0.0f) || !(ty >= 0.0f) ||
        !(tx < (float)g_width) || !(ty < (float)g_height))
        return 1;  // Out of bounds = solid

    return g_collision[(int)ty * g_width + (int)tx];
}

/**
 * Test the four corners of an axis-aligned box for collision.
 *
 * @param half_size  Box half-width and half-height in pixels.
 * @return           1 when any corner is solid, or 0 otherwise.
 */
int world_collision_check_box(float x, float y, float half_size) {
    return world_collision_check(x - half_size, y - half_size) ||
           world_collision_check(x + half_size, y - half_size) ||
           world_collision_check(x - half_size, y + half_size) ||
           world_collision_check(x + half_size, y + half_size);
}

/** Refuse collision paths requiring more than this many samples. */
#define COLLISION_PATH_MAX_STEPS 256

/**
 * Test an axis-aligned box along a movement segment.
 *
 * Samples at half-tile intervals, excludes the origin, and fails closed for invalid or excessive paths.
 *
 * @param x0         Origin X coordinate in pixels.
 * @param y0         Origin Y coordinate in pixels.
 * @param x1         Destination X coordinate in pixels.
 * @param y1         Destination Y coordinate in pixels.
 * @param half_size  Box half-width and half-height in pixels.
 * @return           1 when the path collides or is invalid, or 0 when clear.
 */
int world_collision_check_box_path(float x0, float y0,
                                   float x1, float y1,
                                   float half_size) {
    if (!g_loaded) return 1;
    if (!isfinite(x0) || !isfinite(y0) || !isfinite(x1) || !isfinite(y1))
        return 1;

    float dx = x1 - x0;
    float dy = y1 - y0;
    float distance = sqrtf(dx * dx + dy * dy);

    // sample at half-tile intervals to prevent tunneling
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

/**
 * Release the loaded collision layer.
 */
void world_collision_shutdown(void) {
    free(g_collision);
    g_collision = NULL;
    g_loaded = 0;
}
