/**
 * @file
 * Check layer assignment, collision, and the spawn point.
 */
#include "world/worldgen.h"
#include "world/tile_palette.h"

#include <assert.h>
#include <stdio.h>

int main(void) {
    WorldGenTile t;

    // Ocean is solid and drawn on the base layer only.
    worldgen_tile_at(5, 5, &t);
    assert(t.collision == 1);
    assert(t.base == PAL_DEEP_OCEAN || t.base == PAL_SHALLOW_WATER);
    assert(t.overlay_floor == PAL_EMPTY);
    assert(t.overlay_above == PAL_EMPTY);

    // Open wilderness is always painted, and solid only where mountains rise.
    worldgen_tile_at(2000, 3000, &t);
    assert(t.base != PAL_EMPTY);
    assert(t.collision == ((t.base == PAL_MOUNTAIN_ROCK) ? 1 : 0));

    // Walkable wilderness genuinely exists — the continent is not all mountain.
    int walkable_land = 0;
    for (int i = 0; i < 500; i++) {
        worldgen_tile_at(2000 + i * 7, 3000 + i * 3, &t);
        if (t.collision == 0) walkable_land++;
    }
    assert(walkable_land > 400);

    // City walls are solid.
    worldgen_tile_at(K1_X + CAPITAL_HALF, K1_Y + CAPITAL_HALF, &t);
    assert(t.collision == 1);
    assert(t.base == PAL_CITY_WALL);

    // Roads are walkable road surface.
    worldgen_tile_at(K1_X, K1_Y + 300, &t);
    assert(t.collision == 0);
    assert(t.base == PAL_ROAD);

    // The spawn tile is walkable plaza in the courtyard.
    worldgen_tile_at(ENNARA_X, ENNARA_Y, &t);
    assert(t.collision == 0);
    assert(t.base == PAL_PLAZA_STONE);

    // Buildings are solid, and put floor, wall, and roof on separate layers.
    int found_edge = 0, found_interior = 0;
    for (int y = ENNARA_Y - CAPITAL_HALF; y <= ENNARA_Y + CAPITAL_HALF && !(found_edge && found_interior); y += 3)
        for (int x = ENNARA_X - CAPITAL_HALF; x <= ENNARA_X + CAPITAL_HALF; x += 3) {
            BuildingPart p = worldgen_building_at(x, y);
            if (p == BUILDING_NONE) continue;
            worldgen_tile_at(x, y, &t);
            assert(t.collision == 1);
            assert(t.overlay_floor == PAL_BUILDING_FLOOR);
            assert(t.overlay_above == PAL_BUILDING_ROOF);
            if (p == BUILDING_EDGE) {
                assert(t.overlay_interior == PAL_BUILDING_WALL);
                found_edge = 1;
            } else {
                assert(t.overlay_interior == PAL_EMPTY);
                found_interior = 1;
            }
            if (found_edge && found_interior) break;
        }
    assert(found_edge && found_interior);

    // Every palette value the generator emits is drawable.
    for (int i = 0; i < 4000; i++) {
        int x = (i * 3779) % WORLDGEN_WIDTH;
        int y = (i * 6151) % WORLDGEN_HEIGHT;
        worldgen_tile_at(x, y, &t);
        assert(t.base < PAL_COUNT);
        assert(t.overlay_floor < PAL_COUNT);
        assert(t.overlay_interior < PAL_COUNT);
        assert(t.overlay_above < PAL_COUNT);
        assert(t.collision <= 1);
        assert(t.base != PAL_EMPTY);   // base layer is always painted
    }

    // The spawn point matches the documented Ennara courtyard centre.
    float sx = 0.0f, sy = 0.0f;
    worldgen_spawn_point(&sx, &sy);
    assert(sx == (float)ENNARA_X * WORLDGEN_TILE_PX);
    assert(sy == (float)ENNARA_Y * WORLDGEN_TILE_PX);

    printf("worldgen_tile_test: OK\n");
    return 0;
}
