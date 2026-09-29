/**
 * @file
 * Check the 3D structures grown from the collision layer, without OpenGL.
 *
 * The rule that matters is that what is drawn solid is exactly what the server
 * treats as solid: a wall drawn a tile off lets players walk "through" it, or
 * stop in front of nothing. So the cases check where geometry lands, tile for
 * tile, as well as its shape: walls only on outside edges, a roof that rises
 * to a hip and meets the walls at the eaves, and nothing at all on open
 * ground -- including where a building crosses a chunk border.
 */

#include "world/structure_mesh.h"
#include "world/tile_palette.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int g_failures = 0;

#define CHECK(cond, what)                                                   \
    do {                                                                    \
        if (cond) {                                                         \
            printf("  ok   %s\n", (what));                                  \
        } else {                                                            \
            printf("  FAIL %s  (%s:%d)\n", (what), __FILE__, __LINE__);     \
            g_failures++;                                                   \
        }                                                                   \
    } while (0)

#define MAP_W 96
#define MAP_H 64
#define TILE 16

static StructureKind g_map[MAP_H][MAP_W];
static uint16_t      g_style[MAP_H][MAP_W];
static ChunkVertex g_verts[STRUCTURE_MESH_MAX_VERTS];

static StructureTile sample(void* ctx, int tx, int ty) {
    (void)ctx;
    StructureTile t = { STRUCTURE_NONE, 0 };
    if (tx < 0 || ty < 0 || tx >= MAP_W || ty >= MAP_H) return t;
    t.kind  = g_map[ty][tx];
    t.style = g_style[ty][tx] ? g_style[ty][tx] : PAL_CITY_WALL;
    return t;
}

static void clear_map(void) {
    memset(g_map, 0, sizeof(g_map));
    memset(g_style, 0, sizeof(g_style));
}

static void fill(int x0, int y0, int w, int h, StructureKind k) {
    for (int y = y0; y < y0 + h; y++)
        for (int x = x0; x < x0 + w; x++) g_map[y][x] = k;
}

static StructureMeshInput input_for(int cx, int cy) {
    StructureMeshInput in = { cx, cy, MAP_W, MAP_H, TILE, sample, NULL };
    return in;
}

/** Whether every vertex lies inside the given tile rectangle (world units). */
static int all_inside(const ChunkVertex* v, int n, int tx0, int ty0, int tx1, int ty1) {
    for (int i = 0; i < n; i++) {
        if (v[i].x < tx0 * TILE - 0.01f || v[i].x > tx1 * TILE + 0.01f) return 0;
        if (v[i].y < ty0 * TILE - 0.01f || v[i].y > ty1 * TILE + 0.01f) return 0;
    }
    return 1;
}

static float max_height(const ChunkVertex* v, int n) {
    float m = 0.0f;
    for (int i = 0; i < n; i++) if (v[i].h > m) m = v[i].h;
    return m;
}

int main(void) {
    printf("=== structure mesh ===\n");

    printf("\nTEST 1: only solid tiles become structures\n");
    CHECK(structure_tile_of(PAL_PLAZA_STONE, 0, 0).kind == STRUCTURE_NONE, "open plaza is flat");
    CHECK(structure_tile_of(PAL_DIST_MARKET, PAL_BUILDING_ROOF, 1).kind == STRUCTURE_BUILDING,
          "a solid tile under a roof is a building");
    CHECK(structure_tile_of(PAL_DIST_MARKET, PAL_BUILDING_ROOF, 0).kind == STRUCTURE_NONE,
          "a roof over walkable ground is not raised (D4)");
    CHECK(structure_tile_of(PAL_CITY_WALL, 0, 1).kind == STRUCTURE_BLOCK, "the city wall");
    CHECK(structure_tile_of(PAL_COURTYARD_WALL, 0, 1).kind == STRUCTURE_BLOCK, "the courtyard wall");
    CHECK(structure_tile_of(PAL_MOUNTAIN_ROCK, 0, 1).kind == STRUCTURE_ROCK, "mountain rock");
    CHECK(structure_tile_of(PAL_COLUMN, 0, 1).kind == STRUCTURE_COLUMN &&
          structure_tile_of(PAL_LAMP_POST, 0, 1).kind == STRUCTURE_LAMP &&
          structure_tile_of(PAL_TREE, 0, 1).kind == STRUCTURE_TREE, "the courtyard props");
    CHECK(structure_tile_of(PAL_COLUMN, 0, 0).kind == STRUCTURE_NONE,
          "a prop colour on open ground raises nothing: only collision stands");
    CHECK(structure_tile_of(PAL_DEEP_OCEAN, 0, 1).kind == STRUCTURE_NONE,
          "solid water stays flat: it is a floor you cannot walk on, not a wall");

    printf("\nTEST 2: an empty chunk builds nothing\n");
    clear_map();
    StructureMeshInput in = input_for(0, 0);
    CHECK(structure_mesh_build(g_verts, &in) == 0, "no vertices");

    printf("\nTEST 3: a building stands exactly on its footprint\n");
    clear_map();
    fill(4, 6, 8, 6, STRUCTURE_BUILDING);
    int n = structure_mesh_build(g_verts, &in);
    CHECK(n > 0 && n % 3 == 0, "whole triangles");
    CHECK(all_inside(g_verts, n, 4, 6, 12, 12), "no vertex outside the solid tiles");
    /* 48 tops of 2 triangles, and 2*(8+6) = 28 outside edges of 2 triangles. */
    CHECK(n == (48 * 2 + 28 * 2) * 3, "a top per tile and a wall per outside edge only");

    printf("\nTEST 4: the roof is a hip, meeting the walls at the eaves\n");
    CHECK(fabsf(structure_roof_height(&in, 4, 6) - STRUCTURE_BUILDING_EAVE) < 1e-4f,
          "the roof corner is at the eave");
    CHECK(fabsf(structure_roof_height(&in, 8, 6) - STRUCTURE_BUILDING_EAVE) < 1e-4f,
          "the middle of an edge is at the eave");
    CHECK(fabsf(structure_roof_height(&in, 5, 7) - (STRUCTURE_BUILDING_EAVE + STRUCTURE_ROOF_RISE)) < 1e-4f,
          "one tile in, the roof has risen one step");
    float ridge = structure_roof_height(&in, 8, 9);
    CHECK(fabsf(ridge - (STRUCTURE_BUILDING_EAVE + 3.0f * STRUCTURE_ROOF_RISE)) < 1e-4f,
          "the ridge of a six-deep building is three steps up");
    CHECK(fabsf(max_height(g_verts, n) - ridge) < 1e-4f, "nothing is taller than the ridge");

    printf("\nTEST 5: a building across a chunk border is one building\n");
    clear_map();
    fill(28, 4, 8, 8, STRUCTURE_BUILDING);   /* tiles 28..35: chunk 0 has 28..31 */
    StructureMeshInput left = input_for(0, 0), right = input_for(1, 0);
    int nl = structure_mesh_build(g_verts, &left);
    int nr_ok = 1;
    CHECK(all_inside(g_verts, nl, 28, 4, 32, 12), "the left half stays in the left chunk");
    /* A wall along the border would be a triangle lying wholly in x = 32. */
    for (int i = 0; i + 2 < nl; i += 3) {
        int on_border = 1;
        for (int c = 0; c < 3; c++)
            if (fabsf(g_verts[i + c].x - 32.0f * TILE) > 0.01f) on_border = 0;
        if (on_border) nr_ok = 0;
    }
    CHECK(nr_ok, "no wall is raised along the chunk border inside the building");
    int nr = structure_mesh_build(g_verts, &right);
    CHECK(nr == nl, "both halves of a symmetric building build the same amount");
    CHECK(fabsf(structure_roof_height(&left, 32, 8) -
                (STRUCTURE_BUILDING_EAVE + 4.0f * STRUCTURE_ROOF_RISE)) < 1e-4f,
          "the roof peaks on the border, seen from either chunk");

    printf("\nTEST 6: the city wall is tall and flat, rock rises from the ground\n");
    clear_map();
    fill(0, 20, 32, 2, STRUCTURE_BLOCK);
    n = structure_mesh_build(g_verts, &in);
    CHECK(n > 0 && fabsf(max_height(g_verts, n) - STRUCTURE_CITY_WALL_HEIGHT) < 1e-4f,
          "the rampart is its full height");
    clear_map();
    fill(10, 10, 6, 6, STRUCTURE_ROCK);
    n = structure_mesh_build(g_verts, &in);
    int edge_on_ground = 1;
    for (int i = 0; i < n; i++) {
        int on_edge = fabsf(g_verts[i].x - 10 * TILE) < 0.01f || fabsf(g_verts[i].x - 16 * TILE) < 0.01f ||
                      fabsf(g_verts[i].y - 10 * TILE) < 0.01f || fabsf(g_verts[i].y - 16 * TILE) < 0.01f;
        if (on_edge && g_verts[i].h > 0.01f) edge_on_ground = 0;
    }
    CHECK(n > 0 && edge_on_ground, "rock meets the ground at its edge");
    CHECK(max_height(g_verts, n) >= STRUCTURE_ROCK_BASE, "and rises inside it");

    printf("\nTEST 7: a sunlit face is brighter than a shaded one\n");
    clear_map();
    fill(4, 4, 1, 1, STRUCTURE_BLOCK);
    n = structure_mesh_build(g_verts, &in);
    /* Order: top (6), north, south, west, east (6 each). The sun is in the
     * north-west, so the north face outshines the south one. */
    CHECK(n == 30, "one tile of wall is a top and four sides");
    CHECK(g_verts[8].r > g_verts[14].r, "the north face is lit, the south face shaded");
    CHECK(g_verts[0].r >= g_verts[8].r, "the top is the brightest");

    printf("\nTEST 8: a lower block against a higher one shows only the step\n");
    clear_map();
    fill(4, 4, 3, 1, STRUCTURE_BLOCK);
    g_style[4][6] = PAL_GATE_TOWER;          /* the east tile is a tower */
    g_style[4][4] = g_style[4][5] = PAL_COURTYARD_WALL;
    n = structure_mesh_build(g_verts, &in);
    int step_ok = 0;
    for (int i = 0; i + 2 < n; i += 3) {
        /* The tower's west face rises from the wall's top, not the ground. */
        if (fabsf(g_verts[i].x - 6.0f * TILE) < 0.01f && fabsf(g_verts[i + 1].x - 6.0f * TILE) < 0.01f &&
            fabsf(g_verts[i + 2].x - 6.0f * TILE) < 0.01f) {
            float lo = fminf(g_verts[i].h, fminf(g_verts[i + 1].h, g_verts[i + 2].h));
            if (fabsf(lo - STRUCTURE_COURTYARD_WALL_HEIGHT) < 1e-3f) step_ok = 1;
            if (lo < STRUCTURE_COURTYARD_WALL_HEIGHT - 1e-3f) { step_ok = 0; break; }
        }
    }
    CHECK(step_ok, "the tower's face against the wall starts at the wall's top");

    printf("\nTEST 9: a prop is one model per footprint, standing on it\n");
    clear_map();
    fill(30, 8, 2, 2, STRUCTURE_COLUMN);     /* straddles chunks 0 and 1 */
    int n0 = structure_mesh_build(g_verts, &left);
    CHECK(n0 > 0, "the chunk holding the footprint's north-west tile builds it");
    int base_inside = 1;
    for (int i = 0; i < n0; i++)
        if (g_verts[i].h < 0.01f &&
            (g_verts[i].x < 30 * TILE - 0.01f || g_verts[i].x > 32 * TILE + 0.01f ||
             g_verts[i].y < 8 * TILE - 0.01f  || g_verts[i].y > 10 * TILE + 0.01f))
            base_inside = 0;
    CHECK(base_inside, "where it meets the ground, it stays on its solid tiles");
    CHECK(structure_mesh_build(g_verts, &right) == 0, "the neighbouring chunk does not build it twice");
    clear_map();
    fill(10, 10, 2, 2, STRUCTURE_TREE);
    fill(20, 10, 1, 1, STRUCTURE_LAMP);
    n = structure_mesh_build(g_verts, &in);
    CHECK(n > 0 && max_height(g_verts, n) > 60.0f, "trees and lamps stand taller than a character");

    printf("\n%s: %d failure(s)\n", g_failures ? "FAILED" : "PASSED", g_failures);
    return g_failures ? 1 : 0;
}
