/**
 * @file
 * Lay out the Ennara courtyard: the small walled square every new character
 * wakes up in.
 *
 * The story puts a rift in the middle ("opens, puts somebody down in this
 * courtyard, closes again"), the Warden beside it, the leaders of the nine
 * peoples on a ring around them, and a sentry on the south gate. This file
 * gives that a place: a paved centre with the rift's scar, lawns cut by paths,
 * a walkway along the leaders' ring, lamps, a colonnade, trees, and a low wall
 * with a gate wherever a road leaves.
 *
 * Coordinates matter here. The NPCs stand at fixed world positions
 * (Story/gen_opening.py writes spawns.json from the same city centre), and the
 * server validates every step against this file's collision. So every solid
 * feature sits clear of the NPC positions and of the straight lines between
 * the spawn and them, and the spawn tile itself stays open plaza. What is solid
 * here is drawn solid by the client's structure_mesh.c, tile for tile.
 *
 * Distances are in tiles from the courtyard centre. The leaders' ring is a
 * fixed 800 px (50 tiles) in gen_opening.py, not scaled with the world, so the
 * features around it are fixed too; only the wall follows COURTYARD_RADIUS.
 */
#include "world/worldgen.h"
#include "world/tile_palette.h"

#include <math.h>
#include <stdlib.h>

#define PI_F 3.14159265f

/** The rift's scar, around the spawn tile (which stays plain plaza). */
#define RIFT_GLOW_INNER   2.5f
#define RIFT_GLOW_OUTER   3.5f
#define RIFT_OUTER        6.0f

/** Inlaid stone ring around the paved centre. */
#define CENTRE_INLAY_INNER 14.0f
#define CENTRE_INLAY_OUTER 15.0f

/** Lamps just outside the paved centre. Each stands midway between two
 *  leaders (who are every 40 degrees from north), so none is on the walk from
 *  the spawn to a leader, and at least 20 degrees off every road axis, so
 *  none is on a road. Six angles satisfy both; they are mirror-symmetric. */
#define LAMP_RADIUS        19.0f
static const float LAMP_DEGREES[] = { 20.0f, 60.0f, 140.0f, 220.0f, 300.0f, 340.0f };
#define LAMP_COUNT ((int)(sizeof(LAMP_DEGREES) / sizeof(LAMP_DEGREES[0])))

/** The walkway the leaders stand on (their ring is 50 tiles). */
#define RING_WALK_INNER    46.0f
#define RING_WALK_OUTER    54.0f

/** A column every 22.5 degrees behind the leaders, offset half a step so the
 *  nearest to any road axis is still 11 tiles off it. Outside the leaders'
 *  ring, so no column can stand between the spawn and a leader. */
#define COLONNADE_RADIUS   59.0f
#define COLONNADE_COUNT    16
#define COLONNADE_FIRST_DEG 11.25f
#define COLUMN_SIZE        2

/** The courtyard wall: two tiles thick on the courtyard's edge, open where
 *  the roads leave, with a tower either side of each gate.
 *
 *  It stops short of the corners. gen_opening.py rings the hostile camps
 *  around the courtyard 1600 px out, which puts the four diagonal camps on its
 *  corners; a closed wall would cut them in half. Broken off there instead,
 *  the wall reads as what Ennara is -- what is still standing. */
#define WALL_THICKNESS     2
#define WALL_CORNER_GAP    13
#define GATE_HALF          (CITY_ROAD_HALF + 1)
#define TOWER_SIZE         4

/** A path inside the wall, so the lawns do not run into the stone. */
#define PERIMETER_PATH     5

/** Trees stand in the four corners, in groups of three. */
#define TREE_SIZE          2

/** Whether (dx, dy) lies in the size x size block whose centre is at polar
 *  (radius, deg), measured clockwise from north (north is -y). */
static int in_block_at(int dx, int dy, float radius, float deg, int size) {
    float a = deg * PI_F / 180.0f;
    int x0 = (int)lroundf(radius * sinf(a)) - size / 2;
    int y0 = (int)lroundf(-radius * cosf(a)) - size / 2;
    return dx >= x0 && dx < x0 + size && dy >= y0 && dy < y0 + size;
}

static int is_lamp(int dx, int dy) {
    for (int i = 0; i < LAMP_COUNT; i++)
        if (in_block_at(dx, dy, LAMP_RADIUS, LAMP_DEGREES[i], 1))
            return 1;
    return 0;
}

static int is_column(int dx, int dy) {
    for (int i = 0; i < COLONNADE_COUNT; i++)
        if (in_block_at(dx, dy, COLONNADE_RADIUS,
                        COLONNADE_FIRST_DEG + 360.0f * i / COLONNADE_COUNT, COLUMN_SIZE))
            return 1;
    return 0;
}

/** Trees: three to a corner, mirrored into all four. */
static int is_tree(int dx, int dy) {
    static const int OFFSETS[3][2] = {
        { COURTYARD_RADIUS - 14, COURTYARD_RADIUS - 24 },
        { COURTYARD_RADIUS - 24, COURTYARD_RADIUS - 14 },
        { COURTYARD_RADIUS - 17, COURTYARD_RADIUS - 17 },
    };
    int ax = abs(dx), ay = abs(dy);
    for (int i = 0; i < 3; i++) {
        if (ax >= OFFSETS[i][0] && ax < OFFSETS[i][0] + TREE_SIZE &&
            ay >= OFFSETS[i][1] && ay < OFFSETS[i][1] + TREE_SIZE)
            return 1;
    }
    return 0;
}

/**
 * The wall and its gate towers, or PAL_EMPTY.
 *
 * `a` runs toward the wall, `b` along it; called once per axis.
 */
static uint16_t wall_on_axis(int a, int b) {
    const int r = COURTYARD_RADIUS;
    a = abs(a);
    b = abs(b);
    if (a > r) return PAL_EMPTY;

    /* Towers flank each gate, standing into the courtyard from the wall. */
    if (a > r - TOWER_SIZE && b > GATE_HALF && b <= GATE_HALF + TOWER_SIZE)
        return PAL_GATE_TOWER;
    if (a > r - WALL_THICKNESS && b > GATE_HALF && b <= r - WALL_CORNER_GAP)
        return PAL_COURTYARD_WALL;
    return PAL_EMPTY;
}

int worldgen_courtyard_at(int x, int y, WorldGenTile* out) {
    const int dx = x - ENNARA_X, dy = y - ENNARA_Y;
    const int r = COURTYARD_RADIUS;
    if (abs(dx) > r || abs(dy) > r) return 0;

    out->overlay_floor = out->overlay_interior = out->overlay_above = PAL_EMPTY;
    out->collision = 0;

    /* Solid features first: the wall, then what stands inside it. */
    uint16_t wall = wall_on_axis(dx, dy);
    if (wall == PAL_EMPTY) wall = wall_on_axis(dy, dx);
    if (wall != PAL_EMPTY) {
        out->base = wall;
        out->collision = 1;
        return 1;
    }
    if (is_column(dx, dy)) { out->base = PAL_COLUMN;    out->collision = 1; return 1; }
    if (is_lamp(dx, dy))   { out->base = PAL_LAMP_POST; out->collision = 1; return 1; }
    if (is_tree(dx, dy))   { out->base = PAL_TREE;      out->collision = 1; return 1; }

    /* The ground. The spawn tile stays plain plaza. */
    float dist = sqrtf((float)(dx * dx + dy * dy));
    int on_road = abs(dx) <= CITY_ROAD_HALF || abs(dy) <= CITY_ROAD_HALF;

    out->base = PAL_PLAZA_STONE;
    if (dx == 0 && dy == 0) return 1;

    if (dist >= RIFT_GLOW_INNER && dist < RIFT_GLOW_OUTER) { out->base = PAL_RIFT_GLOW; return 1; }
    if (dist >= RIFT_GLOW_OUTER && dist < RIFT_OUTER)      { out->base = PAL_RIFT;      return 1; }
    if (dist >= CENTRE_INLAY_INNER && dist < CENTRE_INLAY_OUTER) {
        out->base = PAL_PLAZA_INLAY;
        return 1;
    }
    if (dist >= RING_WALK_INNER && dist < RING_WALK_OUTER) {
        int edge = dist < RING_WALK_INNER + 1.0f || dist >= RING_WALK_OUTER - 1.0f;
        out->base = edge ? PAL_PLAZA_INLAY : PAL_PLAZA_STONE;
        return 1;
    }
    if (on_road || dist < CENTRE_INLAY_INNER) return 1;

    /* Diagonal paths from the centre to the corners, and one inside the wall. */
    int diagonal  = abs(abs(dx) - abs(dy)) <= 2;
    int perimeter = abs(dx) > r - WALL_THICKNESS - PERIMETER_PATH ||
                    abs(dy) > r - WALL_THICKNESS - PERIMETER_PATH;
    if (diagonal || perimeter) return 1;

    out->base = PAL_COURTYARD_GRASS;
    return 1;
}
