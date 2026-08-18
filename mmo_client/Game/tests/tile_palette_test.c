/**
 * @file
 * Check tile palette bounds and colour table integrity.
 */
#include "world/tile_palette.h"

#include <assert.h>
#include <stdio.h>

int main(void) {
    // Index 0 is "empty" and has no colour.
    assert(tile_palette_rgb(PAL_EMPTY) == NULL);

    // Out-of-range indices are rejected rather than read past the table.
    assert(tile_palette_rgb(PAL_COUNT) == NULL);
    assert(tile_palette_rgb(60000) == NULL);

    // Every real entry returns three finite channels in [0,1].
    for (uint16_t i = 1; i < PAL_COUNT; i++) {
        const float* c = tile_palette_rgb(i);
        assert(c != NULL);
        for (int ch = 0; ch < 3; ch++)
            assert(c[ch] >= 0.0f && c[ch] <= 1.0f);
    }

    // Spot-check that neighbouring biome colours are actually distinguishable,
    // so bands do not visually merge.
    const float* sand = tile_palette_rgb(PAL_DESERT_SAND);
    const float* snow = tile_palette_rgb(PAL_SNOW);
    float d = 0.0f;
    for (int ch = 0; ch < 3; ch++) d += (sand[ch] - snow[ch]) * (sand[ch] - snow[ch]);
    assert(d > 0.05f);

    printf("tile_palette_test: OK\n");
    return 0;
}
