#ifndef WORLDGEN_H
#define WORLDGEN_H

#include <stdint.h>

/** Continent dimensions in tiles, and tile edge length in pixels. */
#define WORLDGEN_WIDTH   15400
#define WORLDGEN_HEIGHT   7700
#define WORLDGEN_TILE_PX     16

/** Biome band lower bounds in tile rows; each band runs to the next bound. */
#define BAND_SNOW_END     1771
#define BAND_TROPICAL_END 4697
#define BAND_DESERT_END   6468

/** Maximum vertical wobble applied to band boundaries, in tiles. */
#define BAND_WOBBLE 90.0f

/** Ocean ring thickness at the map edge, in tiles, before wobble. */
#define OCEAN_MARGIN 360
#define OCEAN_WOBBLE 80.0f

/** Capital footprint half-width in tiles; full footprint is twice this. */
#define CAPITAL_HALF 1000

/** Capital centre coordinates in tiles. */
#define K2_X  4600
#define K2_Y   900
#define K1_X  8100
#define K1_Y  3200
#define K3_X  7300
#define K3_Y  6550
#define ENNARA_X 13900
#define ENNARA_Y  5580

/** Extra margin around Ennara forced to desert so the city sits in one biome. */
#define ENNARA_BIOME_MARGIN 200

/** Identify a terrain band. */
typedef enum {
    BIOME_OCEAN = 0,
    BIOME_SNOW,
    BIOME_TROPICAL,
    BIOME_DESERT,
    BIOME_COLD_SOUTH
} BiomeId;

float worldgen_noise(int x, int y, int seed);
float worldgen_noise_octaves(int x, int y, int seed, int octaves);

int     worldgen_is_ocean(int x, int y);
BiomeId worldgen_biome_at(int x, int y);

#endif // WORLDGEN_H
