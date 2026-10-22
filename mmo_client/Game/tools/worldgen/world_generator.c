/**
 * @file
 * Generate the Hana to Taiga continent as a flat-colour world file.
 */
#include "world/worldgen.h"
#include "world_format.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char* argv[]) {
    const char* output = (argc > 1) ? argv[1] : "world.dat";

    int width  = WORLDGEN_WIDTH;
    int height = WORLDGEN_HEIGHT;

    // Optional smaller dimensions for quick smoke runs.
    if (argc > 3) {
        width  = atoi(argv[2]);
        height = atoi(argv[3]);
        if (width <= 0 || height <= 0) {
            fprintf(stderr, "Invalid dimensions\n");
            return 1;
        }
    }

    printf("=== Hana to Taiga World Generator ===\n");
    printf("Output: %s\n", output);
    printf("Size:   %d x %d tiles (%d px tiles)\n",
           width, height, WORLDGEN_TILE_PX);

    /* Per tile: one uint16 per tile layer plus one collision byte. Plus the
     * fixed header. Computed from the format constants rather than from
     * literals, which is how the old estimate came to be wrong. */
    double per_tile = (double)WORLD_FORMAT_TILE_LAYERS * (double)sizeof(uint16_t) + 1.0;
    double bytes = (double)width * height * per_tile
                 + (double)WORLD_FORMAT_PREAMBLE_BYTES + 12.0 + 1.0;
    printf("Expect: %.2f GB\n", bytes / (1024.0 * 1024.0 * 1024.0));

    if (!worldgen_write(output, width, height)) {
        fprintf(stderr, "[WORLDGEN] FAILED to write %s\n", output);
        return 1;
    }

    // Companion overview for the in-game map. The world file is far too large
    // to back a whole-continent view, so the map reads this instead.
    char ov_path[512];
    snprintf(ov_path, sizeof(ov_path), "%s.overview", output);
    if (!worldgen_write_overview(ov_path, width, height,
                                 WORLDGEN_OVERVIEW_SCALE)) {
        fprintf(stderr, "[WORLDGEN] FAILED to write %s\n", ov_path);
        return 1;
    }
    printf("[WORLDGEN] Overview: %s (1/%d scale)\n",
           ov_path, WORLDGEN_OVERVIEW_SCALE);

    float sx = 0.0f, sy = 0.0f;
    worldgen_spawn_point(&sx, &sy);
    printf("[WORLDGEN] Done.\n");
    printf("[WORLDGEN] Spawn (Ennara Courtyard): (%.1f, %.1f) px\n", sx, sy);
    return 0;
}
