/**
 * @file
 * Check the generated world file's header, section sizes, and readability.
 */
#include "world/worldgen.h"
#include "world/tile_palette.h"
#include "world_format.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_W 128
#define TEST_H 96

int main(void) {
    const char* path = "/tmp/worldgen_format_test.dat";
    assert(worldgen_write(path, TEST_W, TEST_H) == 1);

    FILE* f = fopen(path, "rb");
    assert(f);

    /* The preamble that makes this file identifiable and versioned. Without it
     * the server's collision reader had no way to notice that the layout had
     * changed under it, and read tile data as its collision map. */
    char magic[WORLD_FORMAT_MAGIC_LEN];
    assert(fread(magic, 1, sizeof(magic), f) == sizeof(magic));
    assert(memcmp(magic, WORLD_FORMAT_MAGIC, sizeof(magic)) == 0);

    uint32_t version = 0, tile_layers = 0;
    assert(fread(&version,     sizeof(uint32_t), 1, f) == 1);
    assert(fread(&tile_layers, sizeof(uint32_t), 1, f) == 1);
    assert(version == WORLD_FORMAT_VERSION);
    assert(tile_layers == WORLD_FORMAT_TILE_LAYERS);

    int32_t w = 0, h = 0, ts = 0;
    assert(fread(&w,  sizeof(int32_t), 1, f) == 1);
    assert(fread(&h,  sizeof(int32_t), 1, f) == 1);
    assert(fread(&ts, sizeof(int32_t), 1, f) == 1);
    assert(w == TEST_W);
    assert(h == TEST_H);
    assert(ts == WORLDGEN_TILE_PX);

    // Flat-colour worlds declare no tilesets — this is what switches the
    // client renderer into palette mode.
    uint8_t tileset_count = 0xFF;
    assert(fread(&tileset_count, 1, 1, f) == 1);
    assert(tileset_count == 0);

    // Four uint16 layers then one uint8 layer, all fully present.
    size_t tiles = (size_t)TEST_W * TEST_H;
    uint16_t* layer = malloc(tiles * sizeof(uint16_t));
    assert(layer);
    for (uint32_t l = 0; l < tile_layers; l++) {
        assert(fread(layer, sizeof(uint16_t), tiles, f) == tiles);
        for (size_t i = 0; i < tiles; i++)
            assert(layer[i] < PAL_COUNT);
    }
    free(layer);

    uint8_t* collision = malloc(tiles);
    assert(collision);
    assert(fread(collision, 1, tiles, f) == tiles);
    for (size_t i = 0; i < tiles; i++)
        assert(collision[i] <= 1);
    free(collision);

    // Nothing trails the collision layer.
    long consumed = ftell(f);
    assert(fseek(f, 0, SEEK_END) == 0);
    assert(ftell(f) == consumed);

    /* Preamble + 3 int32 header + 1 tileset-count byte, then the layers.
     * Written out rather than as a literal so a change to the preamble fails
     * here rather than silently shifting every offset in the file. */
    long expected = WORLD_FORMAT_PREAMBLE_BYTES + 12 + 1
                  + (long)tiles * (long)tile_layers * (long)sizeof(uint16_t)
                  + (long)tiles;
    assert(consumed == expected);

    fclose(f);
    remove(path);

    // A write to an unopenable path must fail cleanly. This drives the early
    // cleanup path, where the later tmp_path[] entries are never populated.
    assert(worldgen_write("/nonexistent_dir_xyz/world.dat", 64, 64) == 0);

    // And it must not strand temp files anywhere.
    assert(system("test -z \"$(ls /tmp/*.layer*.tmp 2>/dev/null)\"") == 0);

    // A failure that occurs BEFORE the destination is opened must leave any
    // previous world.dat intact. Regression: cleanup once ran remove(path)
    // unconditionally, so a full disk or an uncreatable temp file destroyed the
    // last good 1 GB world instead of just declining to replace it.
    assert(system("rm -rf /tmp/wg_keep && mkdir -p /tmp/wg_keep") == 0);
    FILE* prev = fopen("/tmp/wg_keep/world.dat", "wb");
    assert(prev);
    assert(fputs("PREVIOUS", prev) >= 0);
    assert(fclose(prev) == 0);
    // Occupy layer 0's temp name with a directory so fopen(..,"wb") must fail.
    assert(system("mkdir -p /tmp/wg_keep/world.dat.layer0.tmp") == 0);
    assert(worldgen_write("/tmp/wg_keep/world.dat", 64, 64) == 0);
    prev = fopen("/tmp/wg_keep/world.dat", "rb");
    assert(prev);  // must still exist
    char kept[16] = {0};
    assert(fread(kept, 1, 8, prev) == 8);
    assert(fclose(prev) == 0);
    assert(strcmp(kept, "PREVIOUS") == 0);
    assert(system("rm -rf /tmp/wg_keep") == 0);

    printf("worldgen_format_test: OK\n");
    return 0;
}