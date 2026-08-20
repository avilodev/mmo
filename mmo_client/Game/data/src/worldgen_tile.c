/**
 * @file
 * Compose per-tile layer values and collision from biome and city layout.
 */
#include "world/worldgen.h"
#include "world/tile_palette.h"

/**
 * Pick a terrain colour for a land tile, varying it with local noise so each
 * band reads as a cohesive mix rather than a flat slab.
 */
static uint16_t terrain_tile(int x, int y, BiomeId biome) {
    float n = worldgen_noise_octaves(x, y, 2100, 3) * 0.5f + 0.5f;

    // Water is resolved first so ridge noise cannot raise mountains at sea.
    if (biome == BIOME_OCEAN)
        return (n > 0.62f) ? PAL_SHALLOW_WATER : PAL_DEEP_OCEAN;

    float m = worldgen_noise_octaves(x / 3, y / 3, 3100, 2) * 0.5f + 0.5f;

    // Mountains rise out of any land band where the ridge noise peaks.
    if (m > 0.86f) return PAL_MOUNTAIN_ROCK;

    switch (biome) {
        case BIOME_SNOW:
            if (n > 0.78f) return PAL_SNOW_ROCK;
            if (n < 0.24f) return PAL_ICE;
            return PAL_SNOW;

        case BIOME_TROPICAL:
            if (n > 0.66f) return PAL_TROPICAL_JUNGLE;
            if (n < 0.16f) return PAL_BEACH_SAND;
            return PAL_TROPICAL_GRASS;

        case BIOME_DESERT:
            if (n > 0.80f) return PAL_DESERT_ROCK;
            if (n < 0.42f) return PAL_DESERT_DUNE;
            return PAL_DESERT_SAND;

        case BIOME_COLD_SOUTH:
            if (n > 0.74f) return PAL_TUNDRA_FROST;
            return PAL_TUNDRA;

        default:
            return PAL_TUNDRA;
    }
}

/** Map an Ennara district to its ground tint. */
static uint16_t district_tile(DistrictId d) {
    switch (d) {
        case DISTRICT_COURTYARD: return PAL_DIST_COURTYARD;
        case DISTRICT_HARBOR:    return PAL_DIST_HARBOR;
        case DISTRICT_MARKET:    return PAL_DIST_MARKET;
        case DISTRICT_BLESSED:   return PAL_DIST_BLESSED;
        case DISTRICT_HUMAN:     return PAL_DIST_HUMAN;
        case DISTRICT_GUILD:     return PAL_DIST_GUILD;
        default:                 return PAL_EMPTY;
    }
}

/**
 * Compose one tile.
 *
 * Precedence runs walls, then roads, then buildings, then district ground,
 * then open terrain, so built structures always win over the surface below.
 */
void worldgen_tile_at(int x, int y, WorldGenTile* out) {
    out->base             = PAL_EMPTY;
    out->overlay_floor    = PAL_EMPTY;
    out->overlay_interior = PAL_EMPTY;
    out->overlay_above    = PAL_EMPTY;
    out->collision        = 0;

    // Biome classification is the most expensive lookup here, so it is deferred
    // until the built-structure cases below have had their chance to return.
    if (worldgen_is_city_wall(x, y)) {
        out->base      = PAL_CITY_WALL;
        out->collision = 1;
        return;
    }

    if (worldgen_is_road(x, y)) {
        // The courtyard centre reads as plaza rather than road surface.
        int dx = x - ENNARA_X, dy = y - ENNARA_Y;
        int in_courtyard = worldgen_city_at(x, y) == CITY_ENNARA &&
                           dx <= COURTYARD_RADIUS && dx >= -COURTYARD_RADIUS &&
                           dy <= COURTYARD_RADIUS && dy >= -COURTYARD_RADIUS;
        out->base = in_courtyard ? PAL_PLAZA_STONE : PAL_ROAD;
        return;
    }

    BuildingPart part = worldgen_building_at(x, y);
    if (part != BUILDING_NONE) {
        out->base             = district_tile(worldgen_district_at(x, y));
        out->overlay_floor    = PAL_BUILDING_FLOOR;
        out->overlay_interior = (part == BUILDING_EDGE) ? PAL_BUILDING_WALL
                                                        : PAL_EMPTY;
        out->overlay_above    = PAL_BUILDING_ROOF;
        out->collision        = 1;
        return;
    }

    DistrictId district = worldgen_district_at(x, y);
    if (district != DISTRICT_NONE) {
        out->base = (district == DISTRICT_COURTYARD) ? PAL_PLAZA_STONE
                                                     : district_tile(district);
        return;
    }

    // Capitals with no interior design yet are cleared, walkable ground.
    if (worldgen_city_at(x, y) != CITY_NONE) {
        out->base = PAL_PLAZA_STONE;
        return;
    }

    BiomeId biome  = worldgen_biome_at(x, y);
    out->base      = terrain_tile(x, y, biome);
    out->collision = (biome == BIOME_OCEAN || out->base == PAL_MOUNTAIN_ROCK)
                   ? 1 : 0;
}

/**
 * Report the player spawn position in world pixels.
 */
void worldgen_spawn_point(float* out_x, float* out_y) {
    world_city_center_px(world_city_find(CITY_ENNARA), out_x, out_y);
}
