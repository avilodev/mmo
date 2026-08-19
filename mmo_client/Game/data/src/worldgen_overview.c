/**
 * @file
 * Write a downscaled palette overview of the world for the in-game map.
 *
 * The full world file is ~1 GB and streamed a chunk at a time, so it cannot
 * back a whole-continent map view. This companion file samples one base-layer
 * palette index per NxN block, which at the default scale is under half a
 * megabyte and can be loaded once at startup and drawn directly.
 */
#include "world/worldgen.h"

#include <stdio.h>
#include <stdlib.h>

/**
 * Write the overview file.
 *
 * Layout: `int32 overview_width`, `int32 overview_height`, `int32 scale`, then
 * `uint8[overview_width * overview_height]` base-layer palette indices in row
 * order. Cell (cx, cy) samples the world tile at (cx * scale, cy * scale);
 * nearest-sampling is enough because roads and walls are far wider than one
 * cell at the default scale.
 *
 * @param path    Destination file path.
 * @param width   World width in tiles; must be positive.
 * @param height  World height in tiles; must be positive.
 * @param scale   Tiles per overview cell; must be positive.
 * @return        1 on success, or 0 on allocation or I/O failure.
 */
int worldgen_write_overview(const char* path, int width, int height, int scale) {
    if (!path || width <= 0 || height <= 0 || scale <= 0) return 0;

    // Round up so the final partial block still gets a cell.
    int ow = (width  + scale - 1) / scale;
    int oh = (height + scale - 1) / scale;

    FILE*    out        = NULL;
    uint8_t* row        = NULL;
    int      out_opened = 0;
    int      ok         = 0;

    row = malloc((size_t)ow);
    if (!row) goto cleanup;

    out = fopen(path, "wb");
    if (!out) goto cleanup;
    out_opened = 1;  // past this point the old file is already truncated

    int32_t header[3] = { ow, oh, scale };
    if (fwrite(header, sizeof(int32_t), 3, out) != 3) goto cleanup;

    for (int cy = 0; cy < oh; cy++) {
        int wy = cy * scale;
        if (wy >= height) wy = height - 1;

        for (int cx = 0; cx < ow; cx++) {
            int wx = cx * scale;
            if (wx >= width) wx = width - 1;

            WorldGenTile t;
            worldgen_tile_at(wx, wy, &t);

            // Overlays sit on top of terrain, so prefer the most visible one:
            // an above-layer roof reads better on a map than the floor beneath.
            uint16_t v = t.base;
            if (t.overlay_floor)    v = t.overlay_floor;
            if (t.overlay_interior) v = t.overlay_interior;
            if (t.overlay_above)    v = t.overlay_above;

            row[cx] = (uint8_t)v;
        }

        if (fwrite(row, 1, (size_t)ow, out) != (size_t)ow) goto cleanup;
    }

    if (fclose(out) != 0) { out = NULL; goto cleanup; }
    out = NULL;
    ok = 1;

cleanup:
    if (out) { fclose(out); }
    // Same rule as worldgen_write: only discard the destination if this call
    // actually opened it, so an early failure leaves any previous file intact.
    if (!ok && out_opened) remove(path);
    free(row);
    return ok;
}
