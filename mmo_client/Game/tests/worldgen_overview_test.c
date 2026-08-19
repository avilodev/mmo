/**
 * @file
 * Check the overview file's header, size, sampling, and failure handling.
 */
#include "world/worldgen.h"
#include "world/tile_palette.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_W 1000
#define TEST_H  700
#define TEST_S    16

int main(void) {
    const char* path = "/tmp/worldgen_overview_test.dat";
    assert(worldgen_write_overview(path, TEST_W, TEST_H, TEST_S) == 1);

    FILE* f = fopen(path, "rb");
    assert(f);

    int32_t ow = 0, oh = 0, scale = 0;
    assert(fread(&ow,    sizeof(int32_t), 1, f) == 1);
    assert(fread(&oh,    sizeof(int32_t), 1, f) == 1);
    assert(fread(&scale, sizeof(int32_t), 1, f) == 1);

    // Dimensions round UP so a partial trailing block still gets a cell.
    assert(ow == (TEST_W + TEST_S - 1) / TEST_S);
    assert(oh == (TEST_H + TEST_S - 1) / TEST_S);
    assert(scale == TEST_S);

    size_t cells = (size_t)ow * oh;
    uint8_t* buf = malloc(cells);
    assert(buf);
    assert(fread(buf, 1, cells, f) == cells);

    // Every cell must be a real palette entry, and the map must not be blank.
    int nonempty = 0;
    for (size_t i = 0; i < cells; i++) {
        assert(buf[i] < PAL_COUNT);
        if (buf[i] != PAL_EMPTY) nonempty++;
    }
    assert(nonempty == (int)cells);  // base layer is never empty

    // Nothing trails the payload.
    long consumed = ftell(f);
    assert(fseek(f, 0, SEEK_END) == 0);
    assert(ftell(f) == consumed);
    assert(consumed == 12 + (long)cells);
    assert(fclose(f) == 0);

    // Sampling agrees with worldgen_tile_at at the sampled coordinate.
    for (int cy = 0; cy < oh; cy += 7) {
        for (int cx = 0; cx < ow; cx += 7) {
            WorldGenTile t;
            worldgen_tile_at(cx * TEST_S, cy * TEST_S, &t);
            uint16_t expect = t.base;
            if (t.overlay_floor)    expect = t.overlay_floor;
            if (t.overlay_interior) expect = t.overlay_interior;
            if (t.overlay_above)    expect = t.overlay_above;
            assert(buf[(size_t)cy * ow + cx] == (uint8_t)expect);
        }
    }
    free(buf);

    // Deterministic: a second write is byte-identical.
    const char* path2 = "/tmp/worldgen_overview_test2.dat";
    assert(worldgen_write_overview(path2, TEST_W, TEST_H, TEST_S) == 1);
    assert(system("cmp -s /tmp/worldgen_overview_test.dat /tmp/worldgen_overview_test2.dat") == 0);

    // Invalid arguments are refused rather than producing a bad file.
    assert(worldgen_write_overview(NULL, TEST_W, TEST_H, TEST_S) == 0);
    assert(worldgen_write_overview(path, 0, TEST_H, TEST_S) == 0);
    assert(worldgen_write_overview(path, TEST_W, TEST_H, 0)  == 0);

    // An unwritable destination fails without leaving a file behind.
    assert(worldgen_write_overview("/nonexistent_dir_xyz/ov.dat", 64, 64, 8) == 0);

    // A failure before the destination is opened must keep the previous file.
    assert(system("rm -rf /tmp/ov_keep && mkdir -p /tmp/ov_keep") == 0);
    FILE* prev = fopen("/tmp/ov_keep/ov.dat", "wb");
    assert(prev);
    assert(fputs("PREVIOUS", prev) >= 0);
    assert(fclose(prev) == 0);
    assert(worldgen_write_overview("/tmp/ov_keep/ov.dat", 64, 64, 0) == 0);
    prev = fopen("/tmp/ov_keep/ov.dat", "rb");
    assert(prev);
    char kept[16] = {0};
    assert(fread(kept, 1, 8, prev) == 8);
    assert(fclose(prev) == 0);
    assert(strcmp(kept, "PREVIOUS") == 0);
    assert(system("rm -rf /tmp/ov_keep") == 0);

    assert(remove(path)  == 0);
    assert(remove(path2) == 0);

    printf("worldgen_overview_test: OK\n");
    return 0;
}
