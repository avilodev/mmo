/**
 * @file
 * Lay out capital footprints, walls, roads, Ennara districts, and buildings.
 */
#include "world/worldgen.h"

#include <stdlib.h>

/** Describe one capital's centre and identity. */
typedef struct {
    int    cx;
    int    cy;
    CityId id;
} CapitalDef;

static const CapitalDef CAPITALS[] = {
    { K1_X, K1_Y, CITY_K1 },
    { K2_X, K2_Y, CITY_K2 },
    { K3_X, K3_Y, CITY_K3 },
    { ENNARA_X, ENNARA_Y, CITY_ENNARA },
};

#define CAPITAL_COUNT ((int)(sizeof(CAPITALS) / sizeof(CAPITALS[0])))

/**
 * Identify the capital containing a tile.
 *
 * @return The capital id, or CITY_NONE outside every footprint.
 */
CityId worldgen_city_at(int x, int y) {
    for (int i = 0; i < CAPITAL_COUNT; i++) {
        if (x >= CAPITALS[i].cx - CAPITAL_HALF &&
            x <= CAPITALS[i].cx + CAPITAL_HALF &&
            y >= CAPITALS[i].cy - CAPITAL_HALF &&
            y <= CAPITALS[i].cy + CAPITAL_HALF)
            return CAPITALS[i].id;
    }
    return CITY_NONE;
}

/**
 * Look up the centre of the capital containing a tile.
 *
 * @return 1 when inside a capital, or 0 otherwise.
 */
static int capital_center(int x, int y, int* out_cx, int* out_cy) {
    for (int i = 0; i < CAPITAL_COUNT; i++) {
        if (x >= CAPITALS[i].cx - CAPITAL_HALF &&
            x <= CAPITALS[i].cx + CAPITAL_HALF &&
            y >= CAPITALS[i].cy - CAPITAL_HALF &&
            y <= CAPITALS[i].cy + CAPITAL_HALF) {
            *out_cx = CAPITALS[i].cx;
            *out_cy = CAPITALS[i].cy;
            return 1;
        }
    }
    return 0;
}

/**
 * Test whether a tile is part of a capital's perimeter wall.
 */
int worldgen_is_city_wall(int x, int y) {
    int cx, cy;
    if (!capital_center(x, y, &cx, &cy)) return 0;

    int dx = abs(x - cx);
    int dy = abs(y - cy);
    int inner = CAPITAL_HALF - CITY_WALL_THICKNESS;

    // A gate gap on each side keeps the capital enterable.
    int gate_half = CITY_ROAD_HALF + 6;
    if (dx <= gate_half || dy <= gate_half) return 0;

    return dx > inner || dy > inner;
}

/**
 * Test whether a tile is road surface inside a capital.
 *
 * Every capital gets a road cross through its centre, extended through the
 * wall gaps so the gates connect to the interior.
 */
int worldgen_is_road(int x, int y) {
    int cx, cy;
    if (!capital_center(x, y, &cx, &cy)) return 0;

    int dx = abs(x - cx);
    int dy = abs(y - cy);

    if (dx <= CITY_ROAD_HALF || dy <= CITY_ROAD_HALF) return 1;

    // Ennara additionally gets a ring road separating its districts.
    if (worldgen_city_at(x, y) == CITY_ENNARA) {
        int ring = CAPITAL_HALF / 2;
        if (abs(dx - ring) <= CITY_ROAD_HALF && dy <= ring + CITY_ROAD_HALF)
            return 1;
        if (abs(dy - ring) <= CITY_ROAD_HALF && dx <= ring + CITY_ROAD_HALF)
            return 1;
    }

    return 0;
}

/**
 * Identify which Ennara district contains a tile.
 *
 * Courtyard occupies the centre and holds the spawn. Harbor takes the seaward
 * east side. The remaining quadrants carry the other four districts.
 *
 * @return The district id, or DISTRICT_NONE outside Ennara.
 */
DistrictId worldgen_district_at(int x, int y) {
    if (worldgen_city_at(x, y) != CITY_ENNARA) return DISTRICT_NONE;

    int dx = x - ENNARA_X;
    int dy = y - ENNARA_Y;

    if (abs(dx) <= COURTYARD_RADIUS && abs(dy) <= COURTYARD_RADIUS)
        return DISTRICT_COURTYARD;

    // Seaward strip, east of the ring road.
    if (dx > CAPITAL_HALF / 2) return DISTRICT_HARBOR;

    if (dx >= 0 && dy <  0) return DISTRICT_MARKET;
    if (dx >= 0 && dy >= 0) return DISTRICT_GUILD;
    if (dx <  0 && dy <  0) return DISTRICT_BLESSED;
    return DISTRICT_HUMAN;
}

/**
 * Determine whether a tile is inside an Ennara building, and where.
 *
 * Buildings sit on a jittered grid so the layout reads as organic rather than
 * planned. Roads, walls, and the courtyard plaza stay clear.
 *
 * @return BUILDING_EDGE on a wall tile, BUILDING_INTERIOR inside, else BUILDING_NONE.
 */
BuildingPart worldgen_building_at(int x, int y) {
    if (worldgen_city_at(x, y) != CITY_ENNARA) return BUILDING_NONE;
    if (worldgen_is_road(x, y) || worldgen_is_city_wall(x, y))
        return BUILDING_NONE;
    if (worldgen_district_at(x, y) == DISTRICT_COURTYARD) return BUILDING_NONE;

    // Locate the grid cell and its jittered building rectangle.
    int cell_x = x / BUILDING_CELL;
    int cell_y = y / BUILDING_CELL;

    float r_size = worldgen_noise(cell_x, cell_y, 9100) * 0.5f + 0.5f;
    float r_ox   = worldgen_noise(cell_x, cell_y, 9200) * 0.5f + 0.5f;
    float r_oy   = worldgen_noise(cell_x, cell_y, 9300) * 0.5f + 0.5f;
    float r_skip = worldgen_noise(cell_x, cell_y, 9400) * 0.5f + 0.5f;

    // Leave roughly a quarter of cells empty so blocks are uneven.
    if (r_skip < 0.25f) return BUILDING_NONE;

    int size = BUILDING_MIN + (int)(r_size * (BUILDING_MAX - BUILDING_MIN));
    int slack = BUILDING_CELL - size;
    if (slack < 0) return BUILDING_NONE;

    int ox = (int)(r_ox * slack);
    int oy = (int)(r_oy * slack);

    int lx = x - cell_x * BUILDING_CELL;
    int ly = y - cell_y * BUILDING_CELL;

    if (lx < ox || lx >= ox + size) return BUILDING_NONE;
    if (ly < oy || ly >= oy + size) return BUILDING_NONE;

    int on_edge = (lx == ox || lx == ox + size - 1 ||
                   ly == oy || ly == oy + size - 1);
    return on_edge ? BUILDING_EDGE : BUILDING_INTERIOR;
}
