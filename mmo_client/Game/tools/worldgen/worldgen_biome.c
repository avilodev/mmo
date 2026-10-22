/**
 * @file
 * Classify tiles into ocean and the four climate bands.
 */
#include "world/worldgen.h"

/**
 * Test whether a tile lies inside a capital footprint expanded by a margin.
 */
static int in_region(int x, int y, int cx, int cy, int half) {
    return x >= cx - half && x <= cx + half &&
           y >= cy - half && y <= cy + half;
}

/**
 * Test whether a tile lies in the bay carved toward Ennara's harbour.
 *
 * The bay reaches west from the eastern ocean to the city's east wall, spanning
 * the harbour's row range. It stops at the footprint so the city never floods.
 */
static int in_ennara_bay(int x, int y) {
    if (in_region(x, y, ENNARA_X, ENNARA_Y, CAPITAL_HALF)) return 0;

    int bay_top    = ENNARA_Y - CAPITAL_HALF / 2;
    int bay_bottom = ENNARA_Y + CAPITAL_HALF / 2;
    if (y < bay_top || y > bay_bottom) return 0;

    // Only east of the city, out to the map edge.
    return x > ENNARA_X + CAPITAL_HALF;
}

/**
 * Test whether a tile is open water.
 *
 * @return 1 for ocean or bay, or 0 for land.
 */
int worldgen_is_ocean(int x, int y) {
    // Capitals are always dry land, whatever the coastline noise does.
    if (in_region(x, y, ENNARA_X, ENNARA_Y, CAPITAL_HALF)) return 0;
    if (in_region(x, y, K1_X, K1_Y, CAPITAL_HALF)) return 0;
    if (in_region(x, y, K2_X, K2_Y, CAPITAL_HALF)) return 0;
    if (in_region(x, y, K3_X, K3_Y, CAPITAL_HALF)) return 0;

    if (in_ennara_bay(x, y)) return 1;

    float wob = worldgen_noise_octaves(x / OCEAN_WOBBLE_CELL, y / OCEAN_WOBBLE_CELL,
                                       8000, 2) * OCEAN_WOBBLE;

    if ((float)x < OCEAN_MARGIN + wob) return 1;
    if ((float)x > (float)(WORLDGEN_WIDTH  - OCEAN_MARGIN) + wob) return 1;
    if ((float)y < OCEAN_MARGIN + wob) return 1;
    if ((float)y > (float)(WORLDGEN_HEIGHT - OCEAN_MARGIN) + wob) return 1;

    return 0;
}

/**
 * Classify a tile's climate band.
 *
 * Ennara overhangs the desert band, so a local bulge forces desert across its
 * footprint and margin rather than letting the city straddle two biomes.
 */
BiomeId worldgen_biome_at(int x, int y) {
    if (worldgen_is_ocean(x, y)) return BIOME_OCEAN;

    if (in_region(x, y, ENNARA_X, ENNARA_Y,
                  CAPITAL_HALF + ENNARA_BIOME_MARGIN))
        return BIOME_DESERT;

    float wob = worldgen_noise_octaves(x / BAND_WOBBLE_CELL, y / BAND_WOBBLE_CELL,
                                       7000, 2) * BAND_WOBBLE;
    float fy  = (float)y + wob;

    if (fy < BAND_SNOW_END)     return BIOME_SNOW;
    if (fy < BAND_TROPICAL_END) return BIOME_TROPICAL;
    if (fy < BAND_DESERT_END)   return BIOME_DESERT;
    return BIOME_COLD_SOUTH;
}
