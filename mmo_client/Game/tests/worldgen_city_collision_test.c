/**
 * @file
 * Check the baked city collision against the places people stand.
 *
 * The footprints come from the COLLISION meshes of the city .blend, and the
 * server walks players against them. A footprint that covers the spawn or an
 * NPC is a character stuck inside a wall, so this reads the spawn point and the
 * server's spawns.json (skipped in a client-only checkout) and fails on any
 * that land on a solid tile.
 */
#include "world/worldgen.h"
#include "world_regions.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SPAWNS_PATH "../mmo_server/world_server/data/spawns.json"

/** The test town's props and walls cover about a thousand tiles; this is a
 *  floor, not a fit. */
#define MIN_SOLID_TILES 500

/** The flat layer worldgen writes: no walkable span means solid, and outside
 *  the baked spans the footprint collision applies. */
static int flat_solid(int x, int y) {
    int spans = worldgen_city_spans_count(x, y);
    return spans >= 0 ? spans == 0 : worldgen_city_collision_solid(x, y);
}

static int solid_at_px(float px, float py) {
    return flat_solid((int)floorf(px / WORLDGEN_TILE_PX),
                      (int)floorf(py / WORLDGEN_TILE_PX));
}

#define REACH_RADIUS 120

/** The player box is 12 units wide, under a tile of 16, so centred on a tile it
 *  overlaps that tile alone, and moving between two open tile centres it
 *  overlaps only those two. A tile is walkable when it is open. */
static int box_clear_tile(int tx, int ty) {
    return !flat_solid(CITY_ENNARA_TILE_X + tx, CITY_ENNARA_TILE_Y + ty);
}

/** Mark every tile a player-sized box can walk to from (sx, sy), in tiles from
 *  the city centre, within REACH_RADIUS. */
static unsigned char* reachable_from(int sx, int sy) {
    const int side = 2 * REACH_RADIUS + 1;
    unsigned char* seen = calloc((size_t)side * side, 1);
    int* queue = malloc((size_t)side * side * 2 * sizeof(int));
    assert(seen && queue);
    int head = 0, tail = 0;
    queue[tail++] = sx; queue[tail++] = sy;
    seen[(sy + REACH_RADIUS) * side + sx + REACH_RADIUS] = 1;
    while (head < tail) {
        int x = queue[head++], y = queue[head++];
        const int d[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
        for (int k = 0; k < 4; k++) {
            int nx = x + d[k][0], ny = y + d[k][1];
            if (abs(nx) > REACH_RADIUS || abs(ny) > REACH_RADIUS) continue;
            unsigned char* cell = &seen[(ny + REACH_RADIUS) * side + nx + REACH_RADIUS];
            if (*cell || !box_clear_tile(nx, ny)) continue;
            *cell = 1;
            queue[tail++] = nx; queue[tail++] = ny;
        }
    }
    free(queue);
    return seen;
}

/** Within this many tiles of an NPC a player can talk to it or strike it. */
#define NPC_REACH_TILES 3

/** A player-sized box can get within NPC_REACH_TILES of tile (tx, ty), in
 *  tiles from the city centre. An NPC in a doorway or against a wall cannot
 *  itself be stood on by a 3x3 box, but must still be reachable. */
static int player_can_get_near(const unsigned char* reach, int tx, int ty) {
    const int side = 2 * REACH_RADIUS + 1;
    for (int j = -NPC_REACH_TILES; j <= NPC_REACH_TILES; j++) {
        for (int i = -NPC_REACH_TILES; i <= NPC_REACH_TILES; i++) {
            int x = tx + i, y = ty + j;
            if (abs(x) > REACH_RADIUS || abs(y) > REACH_RADIUS) continue;
            if (reach[(y + REACH_RADIUS) * side + x + REACH_RADIUS]) return 1;
        }
    }
    return 0;
}

/** Count the "x"/"y" pairs in spawns.json that stand on a solid tile.
 *  Also counts those a player cannot walk to from the spawn.
 *  @return Spawns read, or -1 if the file is not there. */
static int blocked_spawns(int* out_blocked, const unsigned char* reach) {
    FILE* f = fopen(SPAWNS_PATH, "r");
    if (!f) return -1;

    char  line[512];
    float x = 0.0f;
    int   have_x = 0, n = 0;
    *out_blocked = 0;
    while (fgets(line, sizeof(line), f)) {
        char* p;
        if ((p = strstr(line, "\"x\":")) != NULL) {
            x = strtof(p + 4, NULL);
            have_x = 1;
        } else if (have_x && (p = strstr(line, "\"y\":")) != NULL) {
            float y = strtof(p + 4, NULL);
            n++;
            int tx = (int)floorf(x / WORLDGEN_TILE_PX) - CITY_ENNARA_TILE_X;
            int ty = (int)floorf(y / WORLDGEN_TILE_PX) - CITY_ENNARA_TILE_Y;
            if (solid_at_px(x, y) || !player_can_get_near(reach, tx, ty)) {
                printf("spawn at (%.0f, %.0f) is inside or cut off by the city collision\n", x, y);
                (*out_blocked)++;
            }
            have_x = 0;
        }
    }
    fclose(f);
    return n;
}

int main(void) {
    // Nothing is solid until a file is loaded, and a missing file is refused.
    assert(worldgen_city_collision_solid(CITY_ENNARA_TILE_X, CITY_ENNARA_TILE_Y) == 0);
    assert(worldgen_city_collision_load("Game/assets/baked/does_not_exist.bin") == 0);
    assert(worldgen_city_collision_load(CITY_COLLISION_PATH) == 1);
    assert(worldgen_city_spans_load(CITY_SPANS_PATH) == 1);

    // The city is built up: lots of solid tiles near the origin, none far away.
    int solid = 0;
    for (int dy = -800; dy <= 800; dy++)
        for (int dx = -800; dx <= 800; dx++)
            solid += worldgen_city_collision_solid(CITY_ENNARA_TILE_X + dx,
                                                   CITY_ENNARA_TILE_Y + dy);
    assert(solid >= MIN_SOLID_TILES);
    assert(worldgen_city_collision_solid(0, 0) == 0);
    assert(worldgen_city_collision_solid(-1, -1) == 0);
    assert(worldgen_city_collision_solid(CITY_ENNARA_TILE_X + 100000,
                                         CITY_ENNARA_TILE_Y) == 0);

    // Where a new character wakes up, and every NPC, is open ground.
    float sx = 0.0f, sy = 0.0f;
    worldgen_spawn_point(&sx, &sy);
    assert(!solid_at_px(sx, sy));

    // The spawn also has a floor to stand on in the baked spans.
    assert(worldgen_city_spans_count((int)floorf(sx / WORLDGEN_TILE_PX),
                                     (int)floorf(sy / WORLDGEN_TILE_PX)) > 0);

    // A player-sized box fits there: a box overlapping solid ground cannot move.
    const float half = 6.0f;  // PLAYER_HALF_SIZE_PX in player.h
    assert(!solid_at_px(sx - half, sy - half) && !solid_at_px(sx + half, sy - half));
    assert(!solid_at_px(sx - half, sy + half) && !solid_at_px(sx + half, sy + half));

    int stx = (int)floorf(sx / WORLDGEN_TILE_PX) - CITY_ENNARA_TILE_X;
    int sty = (int)floorf(sy / WORLDGEN_TILE_PX) - CITY_ENNARA_TILE_Y;
    unsigned char* reach = reachable_from(stx, sty);
    int blocked = 0;
    int n = blocked_spawns(&blocked, reach);
    free(reach);
    assert(blocked == 0);

    // A load replaces the old grid rather than adding to it.
    worldgen_city_collision_free();
    worldgen_city_spans_free();
    assert(worldgen_city_collision_solid(CITY_ENNARA_TILE_X + 1, CITY_ENNARA_TILE_Y) == 0);

    printf("worldgen_city_collision_test: OK (%d solid tiles near the origin, %s)\n", solid,
           n < 0 ? "no server tree; spawn checks skipped" : "spawns clear");
    return 0;
}
