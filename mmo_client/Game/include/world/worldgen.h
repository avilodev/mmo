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

/** Wall thickness and road width inside capitals, in tiles. */
#define CITY_WALL_THICKNESS 12
#define CITY_ROAD_HALF      14

/** Ennara building block grid pitch and size bounds, in tiles. */
#define BUILDING_CELL     72
#define BUILDING_MIN      24
#define BUILDING_MAX      48
#define COURTYARD_RADIUS 220

/** Identify a capital. */
typedef enum {
    CITY_NONE = 0,
    CITY_K1,
    CITY_K2,
    CITY_K3,
    CITY_ENNARA
} CityId;

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

#endif // WORLDGEN_H
