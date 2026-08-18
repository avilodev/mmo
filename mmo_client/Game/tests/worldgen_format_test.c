/**
 * @file
 * Check the generated world file's header, section sizes, and readability.
 */
#include "world/worldgen.h"
#include "world/tile_palette.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define TEST_W 128
#define TEST_H 96

int main(void) {
    const char* path = "/tmp/worldgen_format_test.dat";
    assert(worldgen_write(path, TEST_W, TEST_H) == 1);

    FILE* f = fopen(path, "rb");
    assert(f);

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
    for (int l = 0; l < 4; l++) {
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

    long expected = 13 + (long)tiles * 8 + (long)tiles;
    assert(consumed == expected);

    fclose(f);
    remove(path);

    printf("worldgen_format_test: OK\n");
    return 0;
}