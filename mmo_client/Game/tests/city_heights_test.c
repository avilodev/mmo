/**
 * @file
 * Check the baked city heights the camera reads.
 */
#include "world/city_heights.h"
#include "world_regions.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>

#define TILE ((float)WORLD_TILE_PX)

int main(void) {
    // Nothing is tall until a file is loaded, and a missing file is refused.
    assert(city_heights_at(NULL, CITY_ENNARA_TILE_X * TILE, CITY_ENNARA_TILE_Y * TILE) == 0.0f);
    assert(city_heights_load("Game/assets/baked/does_not_exist.bin") == 0);
    assert(city_heights_load(CITY_HEIGHTS_PATH) == 1);

    // The city has tall walls and low props: find a tile of each near the origin.
    float tallest = 0.0f, lowest = 1e9f;
    int solid = 0;
    for (int dy = -600; dy <= 600; dy++)
        for (int dx = -600; dx <= 600; dx++) {
            float h = city_heights_at(NULL, (CITY_ENNARA_TILE_X + dx) * TILE + 1.0f,
                                      (CITY_ENNARA_TILE_Y + dy) * TILE + 1.0f);
            if (h <= 0.0f) continue;
            solid++;
            if (h > tallest) tallest = h;
            if (h < lowest) lowest = h;
        }
    assert(solid > 50000);
    assert(tallest > 8.0f * CITY_UNITS_PER_METRE);   // a building
    assert(lowest  < 1.5f * CITY_UNITS_PER_METRE);   // a fence or a bush

    // Where a character wakes up, and far outside the city, nothing is solid.
    float sx = 0.0f, sy = 0.0f;
    world_city_center_px(world_city_find(CITY_ENNARA), &sx, &sy);
    assert(city_heights_at(NULL, sx, sy) == 0.0f);
    assert(city_heights_at(NULL, -5000.0f, -5000.0f) == 0.0f);
    assert(city_heights_at(NULL, (CITY_ENNARA_TILE_X + 100000) * TILE, sy) == 0.0f);

    city_heights_free();
    assert(city_heights_at(NULL, sx, sy) == 0.0f);
    printf("city_heights_test: OK (%d solid tiles, tallest %.0f units, lowest %.0f)\n",
           solid, tallest, lowest);
    return 0;
}
