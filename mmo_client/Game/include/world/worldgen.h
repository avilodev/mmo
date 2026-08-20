#ifndef WORLDGEN_H
#define WORLDGEN_H

#include <stdint.h>

/**
 * The world scale, the capital coordinates, and CityId live in world_regions.h,
 * which the world server shares so respawn and the economy cannot drift from
 * the map the generator writes. Everything below is generation-only detail.
 */
#include "world_regions.h"

/** Continent dimensions in tiles, and tile edge length in pixels. */
#define WORLDGEN_WIDTH   WG_SCALE(15400)
#define WORLDGEN_HEIGHT  WG_SCALE(7700)
#define WORLDGEN_TILE_PX WORLD_TILE_PX

/** Biome band lower bounds in tile rows; each band runs to the next bound. */
#define BAND_SNOW_END     WG_SCALE(1771)
#define BAND_TROPICAL_END WG_SCALE(4697)
#define BAND_DESERT_END   WG_SCALE(6468)

/** Maximum vertical wobble applied to band boundaries, in tiles. */
#define BAND_WOBBLE WG_SCALEF(90.0f)

/** Noise cell size for the band and coast wobble, in tiles. */
#define BAND_WOBBLE_CELL  WG_SCALE(64)
#define OCEAN_WOBBLE_CELL WG_SCALE(32)

/** Ocean ring thickness at the map edge, in tiles, before wobble. */
#define OCEAN_MARGIN WG_SCALE(360)
#define OCEAN_WOBBLE WG_SCALEF(80.0f)

/** Capital footprint half-width in tiles; full footprint is twice this. */
#define CAPITAL_HALF WG_SCALE(1000)

/** Capital centre coordinates in tiles, named from the shared city table. */
#define K1_X  CITY_K1_TILE_X
#define K1_Y  CITY_K1_TILE_Y
#define K2_X  CITY_K2_TILE_X
#define K2_Y  CITY_K2_TILE_Y
#define K3_X  CITY_K3_TILE_X
#define K3_Y  CITY_K3_TILE_Y
#define ENNARA_X CITY_ENNARA_TILE_X
#define ENNARA_Y CITY_ENNARA_TILE_Y

/** Extra margin around Ennara forced to desert so the city sits in one biome. */
#define ENNARA_BIOME_MARGIN WG_SCALE(200)

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

/** Wall thickness and road width inside capitals, in tiles. */
#define CITY_WALL_THICKNESS WG_SCALE(12)
#define CITY_ROAD_HALF      WG_SCALE(14)

/** Extra half-width of the gate gap beyond the road itself, in tiles. */
#define CITY_GATE_MARGIN    WG_SCALE(6)

/** Ennara building block grid pitch and size bounds, in tiles. */
#define BUILDING_CELL    WG_SCALE(72)
#define BUILDING_MIN     WG_SCALE(24)
#define BUILDING_MAX     WG_SCALE(48)
#define COURTYARD_RADIUS WG_SCALE(220)

/** Identify an Ennara district. */
typedef enum {
    DISTRICT_NONE = 0,
    DISTRICT_COURTYARD,
    DISTRICT_HARBOR,
    DISTRICT_MARKET,
    DISTRICT_BLESSED,
    DISTRICT_HUMAN,
    DISTRICT_GUILD
} DistrictId;

/** Identify a tile's role within a building footprint. */
typedef enum {
    BUILDING_NONE = 0,
    BUILDING_INTERIOR,
    BUILDING_EDGE
} BuildingPart;

CityId       worldgen_city_at(int x, int y);
int          worldgen_is_city_wall(int x, int y);
int          worldgen_is_road(int x, int y);
DistrictId   worldgen_district_at(int x, int y);
BuildingPart worldgen_building_at(int x, int y);

/** Carry every layer value and the collision flag for one tile. */
typedef struct {
    uint16_t base;
    uint16_t overlay_floor;
    uint16_t overlay_interior;
    uint16_t overlay_above;
    uint8_t  collision;
} WorldGenTile;

void worldgen_tile_at(int x, int y, WorldGenTile* out);
void worldgen_spawn_point(float* out_x, float* out_y);

int worldgen_write(const char* path, int width, int height);

/**
 * Tiles per overview cell. Scaled with the world so the in-game map keeps the
 * same on-screen resolution: at full size 15400x7700 downscales to 963x482,
 * and at the 1/3 dev scale 5133x2567 downscales to 1027x514 (~500 KB).
 */
#define WORLDGEN_OVERVIEW_SCALE WG_SCALE(16)

int worldgen_write_overview(const char* path, int width, int height, int scale);

#endif // WORLDGEN_H
