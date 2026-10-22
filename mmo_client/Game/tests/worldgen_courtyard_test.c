/**
 * @file
 * Check the courtyard generator's own layout: its features and its gates.
 *
 * This layout no longer decides collision (3d_refactor.md D42, D43); it is kept
 * for when it comes back. Where the NPCs stand is checked against the city
 * scene's collision in worldgen_city_collision_test.
 */
#include "world/worldgen.h"
#include "world/tile_palette.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** The player's collision box half-size, in world pixels (player.c: one tile). */
#define PLAYER_HALF ((float)WORLDGEN_TILE_PX)

static int solid_at_px(float px, float py) {
    WorldGenTile t;
    worldgen_tile_at((int)floorf(px / WORLDGEN_TILE_PX), (int)floorf(py / WORLDGEN_TILE_PX), &t);
    return t.collision;
}

/** Whether a player-sized box can stand at a point. */
static int box_clear(float px, float py) {
    return !solid_at_px(px - PLAYER_HALF, py - PLAYER_HALF) &&
           !solid_at_px(px + PLAYER_HALF, py - PLAYER_HALF) &&
           !solid_at_px(px - PLAYER_HALF, py + PLAYER_HALF) &&
           !solid_at_px(px + PLAYER_HALF, py + PLAYER_HALF) &&
           !solid_at_px(px, py);
}

/** Whether a player can walk the straight line between two points. */
static int line_clear(float x0, float y0, float x1, float y1) {
    float len = sqrtf((x1 - x0) * (x1 - x0) + (y1 - y0) * (y1 - y0));
    int steps = (int)(len / 4.0f) + 1;
    for (int i = 0; i <= steps; i++) {
        float t = (float)i / (float)steps;
        if (!box_clear(x0 + (x1 - x0) * t, y0 + (y1 - y0) * t)) return 0;
    }
    return 1;
}

int main(void) {
    const float cx = (float)ENNARA_X * WORLDGEN_TILE_PX;
    const float cy = (float)ENNARA_Y * WORLDGEN_TILE_PX;
    WorldGenTile t;

    // The spawn tile is open plaza, and a player fits there.
    worldgen_tile_at(ENNARA_X, ENNARA_Y, &t);
    assert(t.collision == 0 && t.base == PAL_PLAZA_STONE);
    assert(box_clear(cx + 8.0f, cy + 8.0f));

    // The courtyard has its features: a wall, columns, lamps, trees, lawn.
    int counts[PAL_COUNT] = { 0 };
    for (int y = ENNARA_Y - COURTYARD_RADIUS; y <= ENNARA_Y + COURTYARD_RADIUS; y++)
        for (int x = ENNARA_X - COURTYARD_RADIUS; x <= ENNARA_X + COURTYARD_RADIUS; x++) {
            worldgen_tile_at(x, y, &t);
            counts[t.base]++;
            // Only features are solid inside the courtyard.
            if (t.collision)
                assert(t.base == PAL_COURTYARD_WALL || t.base == PAL_GATE_TOWER ||
                       t.base == PAL_COLUMN || t.base == PAL_LAMP_POST || t.base == PAL_TREE);
        }
    assert(counts[PAL_COURTYARD_WALL] > 0 && counts[PAL_GATE_TOWER] > 0);
    assert(counts[PAL_COLUMN] == 16 * 4);
    assert(counts[PAL_LAMP_POST] == 6);
    assert(counts[PAL_TREE] == 12 * 4);
    assert(counts[PAL_COURTYARD_GRASS] > 0 && counts[PAL_RIFT] > 0);

    // Every gate is open: a player walks from the spawn out along each road.
    const float out_dist = (float)(COURTYARD_RADIUS + 4) * WORLDGEN_TILE_PX;
    const float dirs[4][2] = { { 0, -1 }, { 1, 0 }, { 0, 1 }, { -1, 0 } };
    for (int i = 0; i < 4; i++)
        assert(line_clear(cx + 8.0f, cy + 8.0f,
                          cx + 8.0f + dirs[i][0] * out_dist, cy + 8.0f + dirs[i][1] * out_dist));

    // The wall is closed away from the gates, on every side.
    for (int i = 0; i < 4; i++) {
        int along = COURTYARD_RADIUS / 2;
        int wx = ENNARA_X + (int)dirs[i][0] * COURTYARD_RADIUS + (dirs[i][0] == 0 ? along : 0);
        int wy = ENNARA_Y + (int)dirs[i][1] * COURTYARD_RADIUS + (dirs[i][1] == 0 ? along : 0);
        worldgen_tile_at(wx, wy, &t);
        assert(t.collision == 1 && t.base == PAL_COURTYARD_WALL);
    }

    // Where the NPCs stand is no longer decided by this layout: the city
    // scene's collision is what is live, and worldgen_city_collision_test
    // checks every spawn against it (3d_refactor.md D43).
    printf("worldgen_courtyard_test: OK\n");
    return 0;
}
