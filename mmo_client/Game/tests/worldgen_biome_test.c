/**
 * @file
 * Check biome band ordering, ocean margins, and the Ennara desert bulge.
 */
#include "world/worldgen.h"

#include <assert.h>
#include <stdio.h>

int main(void) {
    const int mid_x = WORLDGEN_WIDTH / 2;

    // Interior sample rows land in the expected band. Rows are pulled well
    // inside each band so noise wobble on the boundary cannot flip them.
    assert(worldgen_biome_at(mid_x,  800) == BIOME_SNOW);
    assert(worldgen_biome_at(mid_x, 3200) == BIOME_TROPICAL);
    assert(worldgen_biome_at(mid_x, 5500) == BIOME_DESERT);
    assert(worldgen_biome_at(mid_x, 7200) == BIOME_COLD_SOUTH);

    // The map edges are ocean on all four sides.
    assert(worldgen_is_ocean(5, 5));
    assert(worldgen_is_ocean(WORLDGEN_WIDTH - 5, WORLDGEN_HEIGHT - 5));
    assert(worldgen_biome_at(5, 5) == BIOME_OCEAN);

    // The continental interior is never ocean.
    assert(!worldgen_is_ocean(mid_x, WORLDGEN_HEIGHT / 2));

    // Ennara overhangs the desert band by ~117 rows, so without the local
    // bulge its north edge would read tropical. The bulge must force desert
    // across the whole footprint.
    for (int y = ENNARA_Y - CAPITAL_HALF; y <= ENNARA_Y + CAPITAL_HALF; y += 100)
        assert(worldgen_biome_at(ENNARA_X, y) == BIOME_DESERT);

    // The city footprint is never flooded by the bay.
    for (int y = ENNARA_Y - CAPITAL_HALF; y <= ENNARA_Y + CAPITAL_HALF; y += 100)
        for (int x = ENNARA_X - CAPITAL_HALF; x <= ENNARA_X + CAPITAL_HALF; x += 100)
            assert(!worldgen_is_ocean(x, y));

    // Noise is deterministic and bounded.
    assert(worldgen_noise(12, 34, 7) == worldgen_noise(12, 34, 7));
    for (int i = 0; i < 500; i++) {
        float n = worldgen_noise_octaves(i * 13, i * 7, 1234, 4);
        assert(n >= -1.5f && n <= 1.5f);
    }

    // Ocean and biome classification must never disagree: anything is_ocean()
    // calls water must classify as BIOME_OCEAN, or terrain paints land on sea.
    for (int y = 0; y < WORLDGEN_HEIGHT; y += 37)
        for (int x = 0; x < WORLDGEN_WIDTH; x += 41)
            if (worldgen_is_ocean(x, y))
                assert(worldgen_biome_at(x, y) == BIOME_OCEAN);

    // The bay east of Ennara is water on both accessors, right up to the wall.
    for (int x = ENNARA_X + CAPITAL_HALF + 1; x <= ENNARA_X + CAPITAL_HALF + 200; x++) {
        assert(worldgen_is_ocean(x, ENNARA_Y));
        assert(worldgen_biome_at(x, ENNARA_Y) == BIOME_OCEAN);
    }

    printf("worldgen_biome_test: OK\n");
    return 0;
}
