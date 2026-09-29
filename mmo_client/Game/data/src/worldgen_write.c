/**
 * @file
 * Stream generated tiles into the client's five-section world file.
 */
#include "world/worldgen.h"
#include "world_format.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** Number of layer streams written before the collision layer.
 *
 * Recorded in the file header, so a reader seeks past the tile data by what the
 * file says rather than by a constant it was compiled with. Adding a layer here
 * used to make the server's seek land mid-data and load garbage as its
 * collision map; now it is one number that travels with the file. */
#define TILE_LAYERS WORLD_FORMAT_TILE_LAYERS

/**
 * Write the world file.
 *
 * Each layer is streamed to its own temporary file one row at a time, then the
 * temporaries are concatenated. This keeps peak memory at a few row buffers
 * instead of the whole world, and avoids seeking between layer offsets on
 * every row.
 *
 * @param path    Destination file path.
 * @param width   World width in tiles; must be positive.
 * @param height  World height in tiles; must be positive.
 * @return        1 on success, or 0 on allocation or I/O failure.
 */
int worldgen_write(const char* path, int width, int height) {
    if (!path || width <= 0 || height <= 0) return 0;

    FILE*     tmp[TILE_LAYERS + 1] = {0};
    char      tmp_path[TILE_LAYERS + 1][256] = {{0}};
    uint16_t* rows[TILE_LAYERS]    = {0};
    uint8_t*  collision_row        = NULL;
    FILE*     out                  = NULL;
    int       out_opened           = 0;
    int       ok                   = 0;

    for (int l = 0; l < TILE_LAYERS; l++) {
        rows[l] = malloc((size_t)width * sizeof(uint16_t));
        if (!rows[l]) goto cleanup;
    }
    collision_row = malloc((size_t)width);
    if (!collision_row) goto cleanup;

    for (int l = 0; l <= TILE_LAYERS; l++) {
        snprintf(tmp_path[l], sizeof(tmp_path[l]),
                 "%s.layer%d.tmp", path, l);
        tmp[l] = fopen(tmp_path[l], "wb");
        if (!tmp[l]) goto cleanup;
    }

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            WorldGenTile t;
            worldgen_tile_at(x, y, &t);
            rows[0][x]       = t.base;
            rows[1][x]       = t.overlay_floor;
            rows[2][x]       = t.overlay_interior;
            rows[3][x]       = t.overlay_above;
            /* Nothing is solid while the world is the authored city scene
             * (3d_refactor.md D42): the layout's walls and props are no
             * longer drawn, so they must not block anyone either. The
             * layout still says what would be solid (t.collision). */
            collision_row[x] = 0;
        }

        for (int l = 0; l < TILE_LAYERS; l++) {
            if (fwrite(rows[l], sizeof(uint16_t), (size_t)width, tmp[l])
                != (size_t)width) goto cleanup;
        }
        if (fwrite(collision_row, 1, (size_t)width, tmp[TILE_LAYERS])
            != (size_t)width) goto cleanup;

        if (height >= 100 && y % (height / 20) == 0)
            printf("[WORLDGEN] %d%%\n", (y * 100) / height);
    }

    for (int l = 0; l <= TILE_LAYERS; l++) {
        if (fclose(tmp[l]) != 0) { tmp[l] = NULL; goto cleanup; }
        tmp[l] = NULL;
    }

    out = fopen(path, "wb");
    if (!out) goto cleanup;
    out_opened = 1;  // past this point the old file is already truncated

    /* Say what the file is before saying anything about its contents. */
    if (fwrite(WORLD_FORMAT_MAGIC, 1, WORLD_FORMAT_MAGIC_LEN, out)
        != WORLD_FORMAT_MAGIC_LEN) goto cleanup;

    uint32_t version     = WORLD_FORMAT_VERSION;
    uint32_t tile_layers = TILE_LAYERS;
    if (fwrite(&version,     sizeof(uint32_t), 1, out) != 1) goto cleanup;
    if (fwrite(&tile_layers, sizeof(uint32_t), 1, out) != 1) goto cleanup;

    int32_t header[3] = { width, height, WORLDGEN_TILE_PX };
    if (fwrite(header, sizeof(int32_t), 3, out) != 3) goto cleanup;

    uint8_t tileset_count = 0;  // flat-colour world
    if (fwrite(&tileset_count, 1, 1, out) != 1) goto cleanup;

    static unsigned char copy_buf[1 << 20];
    for (int l = 0; l <= TILE_LAYERS; l++) {
        FILE* in = fopen(tmp_path[l], "rb");
        if (!in) goto cleanup;
        size_t n;
        while ((n = fread(copy_buf, 1, sizeof(copy_buf), in)) > 0) {
            if (fwrite(copy_buf, 1, n, out) != n) { fclose(in); goto cleanup; }
        }
        if (ferror(in)) { fclose(in); goto cleanup; }
        fclose(in);
    }

    if (fclose(out) != 0) { out = NULL; goto cleanup; }
    out = NULL;
    ok = 1;

cleanup:
    for (int l = 0; l <= TILE_LAYERS; l++)
        if (tmp[l]) fclose(tmp[l]);
    if (out) fclose(out);
    // Only discard the destination if this call actually opened (and so
    // truncated) it. Failing before that point — a temp file that cannot be
    // created, a full disk — must leave any previous world.dat intact.
    if (!ok && out_opened) remove(path);
    for (int l = 0; l < TILE_LAYERS; l++) free(rows[l]);
    free(collision_row);
    for (int l = 0; l <= TILE_LAYERS; l++) if (tmp_path[l][0]) remove(tmp_path[l]);
    return ok;
}