/**
 * @file
 * Check the courtyard's solid features against the NPCs that live in it.
 *
 * The NPCs stand at fixed world coordinates the server reads from its own
 * spawns.json, and the server validates every step against the collision this
 * generator writes. So a lamp or a column placed a few tiles wrong is not a
 * cosmetic bug: it is an NPC standing inside a wall, or a new player who
 * cannot walk from where they wake up to the person the quest sends them to.
 *
 * The spawns are read from the server tree rather than copied here, so moving
 * an NPC in Story/gen_opening.py re-checks it against the map. A client-only
 * checkout skips that part and says so.
 */
#include "world/worldgen.h"
#include "world/tile_palette.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SPAWNS_PATH "../mmo_server/world_server/data/spawns.json"
#define MAX_SPAWNS  256

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

typedef struct { float x, y; int quest; char name[48]; } Spawn;

/** Read name/x/y/category from spawns.json without a JSON library: the file is
 *  generated, flat, and one field per line. */
static int load_spawns(Spawn* out, int max) {
    FILE* f = fopen(SPAWNS_PATH, "r");
    if (!f) return -1;
    char line[512];
    int n = -1;
    while (fgets(line, sizeof(line), f)) {
        char* p;
        if ((p = strstr(line, "\"name\":")) != NULL) {
            if (++n >= max) break;
            memset(&out[n], 0, sizeof(out[n]));
            char* q = strchr(p + 7, '"');
            if (q) sscanf(q + 1, "%47[^\"]", out[n].name);
        } else if (n >= 0 && (p = strstr(line, "\"x\":")) != NULL) {
            out[n].x = strtof(p + 4, NULL);
        } else if (n >= 0 && (p = strstr(line, "\"y\":")) != NULL) {
            out[n].y = strtof(p + 4, NULL);
        } else if (n >= 0 && strstr(line, "\"category\": \"quest\"")) {
            out[n].quest = 1;
        }
    }
    fclose(f);
    return n + 1;
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

    Spawn spawns[MAX_SPAWNS];
    int n = load_spawns(spawns, MAX_SPAWNS);
    if (n < 0) {
        printf("worldgen_courtyard_test: OK (no server tree at %s; spawn checks skipped)\n",
               SPAWNS_PATH);
        return 0;
    }
    assert(n > 0);

    int quest = 0;
    for (int i = 0; i < n; i++) {
        // Every NPC, hostile or not, stands on open ground. (Its own tile: an
        // NPC is not a player-sized box, and some camps stand by buildings.)
        if (solid_at_px(spawns[i].x, spawns[i].y)) {
            fprintf(stderr, "%s at (%.1f, %.1f) is inside something solid\n",
                    spawns[i].name, spawns[i].x, spawns[i].y);
            assert(0);
        }
        // And a new player can walk straight from the spawn to anyone who talks.
        if (spawns[i].quest) {
            quest++;
            if (!line_clear(cx, cy, spawns[i].x, spawns[i].y)) {
                fprintf(stderr, "the walk from the spawn to %s is blocked\n", spawns[i].name);
                assert(0);
            }
        }
    }
    assert(quest > 0);

    printf("worldgen_courtyard_test: OK (%d spawns, %d with dialogue)\n", n, quest);
    return 0;
}
