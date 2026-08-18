/**
 * @file
 * Check capital footprints, walls, roads, Ennara districts, and buildings.
 */
#include "world/worldgen.h"

#include <assert.h>
#include <stdio.h>

int main(void) {
    // Each capital centre reports its own id; open wilderness reports none.
    assert(worldgen_city_at(ENNARA_X, ENNARA_Y) == CITY_ENNARA);
    assert(worldgen_city_at(K1_X, K1_Y) == CITY_K1);
    assert(worldgen_city_at(K2_X, K2_Y) == CITY_K2);
    assert(worldgen_city_at(K3_X, K3_Y) == CITY_K3);
    assert(worldgen_city_at(2000, 3000) == CITY_NONE);

    // Footprints are exactly 2000 tiles across: the last row inside counts,
    // one tile further out does not.
    assert(worldgen_city_at(K1_X + CAPITAL_HALF,     K1_Y) == CITY_K1);
    assert(worldgen_city_at(K1_X + CAPITAL_HALF + 1, K1_Y) == CITY_NONE);

    // The perimeter is wall, away from the gates; the centre is not.
    assert(worldgen_is_city_wall(K1_X + CAPITAL_HALF, K1_Y + 500));
    assert(!worldgen_is_city_wall(K1_X, K1_Y));

    // Each side has a gate gap on the road axis, so the capital is enterable.
    assert(!worldgen_is_city_wall(K1_X + CAPITAL_HALF, K1_Y));
    assert(!worldgen_is_city_wall(K1_X, K1_Y + CAPITAL_HALF));

    // A road cross runs through every capital centre.
    assert(worldgen_is_road(K1_X, K1_Y + 300));
    assert(worldgen_is_road(K1_X + 300, K1_Y));

    // Districts exist only inside Ennara.
    assert(worldgen_district_at(K1_X, K1_Y) == DISTRICT_NONE);
    assert(worldgen_district_at(2000, 3000) == DISTRICT_NONE);
    assert(worldgen_district_at(ENNARA_X, ENNARA_Y) == DISTRICT_COURTYARD);

    // Harbor sits on the seaward (east) side so it meets the carved bay.
    assert(worldgen_district_at(ENNARA_X + 700, ENNARA_Y) == DISTRICT_HARBOR);

    // All six districts appear somewhere in the footprint.
    int seen[7] = {0};
    for (int y = ENNARA_Y - CAPITAL_HALF; y <= ENNARA_Y + CAPITAL_HALF; y += 25)
        for (int x = ENNARA_X - CAPITAL_HALF; x <= ENNARA_X + CAPITAL_HALF; x += 25) {
            DistrictId d = worldgen_district_at(x, y);
            if (d != DISTRICT_NONE) seen[d] = 1;
        }
    for (int d = DISTRICT_COURTYARD; d <= DISTRICT_GUILD; d++)
        assert(seen[d]);

    // Buildings appear inside Ennara but never outside a city, never on roads,
    // and never on the spawn tile.
    int building_tiles = 0;
    for (int y = ENNARA_Y - CAPITAL_HALF; y <= ENNARA_Y + CAPITAL_HALF; y += 7)
        for (int x = ENNARA_X - CAPITAL_HALF; x <= ENNARA_X + CAPITAL_HALF; x += 7) {
            if (worldgen_building_at(x, y) != BUILDING_NONE) {
                building_tiles++;
                assert(!worldgen_is_road(x, y));
            }
        }
    assert(building_tiles > 0);
    assert(worldgen_building_at(ENNARA_X, ENNARA_Y) == BUILDING_NONE);
    assert(worldgen_building_at(2000, 3000) == BUILDING_NONE);

    // Undesigned capitals stay empty reserved space.
    int k1_buildings = 0;
    for (int y = K1_Y - CAPITAL_HALF; y <= K1_Y + CAPITAL_HALF; y += 7)
        for (int x = K1_X - CAPITAL_HALF; x <= K1_X + CAPITAL_HALF; x += 7)
            if (worldgen_building_at(x, y) != BUILDING_NONE) k1_buildings++;
    assert(k1_buildings == 0);

    printf("worldgen_city_test: OK\n");
    return 0;
}
