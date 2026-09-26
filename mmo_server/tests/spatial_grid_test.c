/**
 * @file
 * Compare spatial-grid proximity results and ordering against a brute-force oracle.
 */

#include "spatial_grid.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** Define the test world, cell size, and entity capacity. */
#define WORLD_W  9600.0f
#define WORLD_H  6400.0f
#define CELL     SPATIAL_GRID_DEFAULT_CELL
#define MAX_ENT  1000

/** Store one brute-force proximity candidate. */
typedef struct { int index; float dist_sq; } Candidate;

/** Order oracle candidates by distance and caller index. */
static int candidate_cmp(const void* a, const void* b) {
    const Candidate* ca = a;
    const Candidate* cb = b;
    if (ca->dist_sq < cb->dist_sq) return -1;
    if (ca->dist_sq > cb->dist_sq) return 1;
    return ca->index - cb->index;   // stable tie-break so comparisons are exact
}

/** Return nearest-first indices using a full point scan. */
static int brute_force(const SpatialPoint* points, int count,
                       float x, float y, float radius,
                       int* out, int max_out) {
    Candidate* all = malloc(sizeof(Candidate) * (size_t)(count > 0 ? count : 1));
    assert(all);
    int found = 0;

    for (int i = 0; i < count; i++) {
        if (!isfinite(points[i].x) || !isfinite(points[i].y)) continue;
        float dx = points[i].x - x;
        float dy = points[i].y - y;
        float d2 = dx * dx + dy * dy;
        if (d2 > radius * radius) continue;
        all[found].index = i;
        all[found].dist_sq = d2;
        found++;
    }

    qsort(all, (size_t)found, sizeof(Candidate), candidate_cmp);

    int n = found < max_out ? found : max_out;
    for (int i = 0; i < n; i++) out[i] = all[i].index;
    free(all);
    return n;
}

static float dist_sq_to(const SpatialPoint* points, int index, float x, float y) {
    float dx = points[index].x - x;
    float dy = points[index].y - y;
    return dx * dx + dy * dy;
}

static unsigned long g_rng = 0x9E3779B9UL;
/** Generate a deterministic pseudo-random float within a range. */
static float rand_range(float lo, float hi) {
    g_rng = g_rng * 6364136223846793005UL + 1442695040888963407UL;
    float unit = (float)((g_rng >> 33) & 0xFFFFFF) / (float)0xFFFFFF;
    return lo + unit * (hi - lo);
}

/**
 * Run spatial-grid correctness and boundary assertions.
 *
 * @return      Zero after all assertions pass.
 */
int main(void) {
    int out[MAX_ENT];
    int expect[MAX_ENT];

    printf("TEST 1: an empty grid finds nothing\n");
    {
        SpatialGrid* grid = spatial_grid_create(WORLD_W, WORLD_H, CELL, MAX_ENT);
        assert(grid);

        SpatialPoint none[1];
        spatial_grid_build(grid, none, 0);

        assert(spatial_grid_count(grid) == 0);
        int n = spatial_grid_query(grid, 100.0f, 100.0f, 800.0f, out, MAX_ENT);
        printf("  found %d (expect 0)\n", n);
        assert(n == 0);

        int cols = 0, rows = 0;
        spatial_grid_dimensions(grid, &cols, &rows);
        printf("  grid is %dx%d cells for a %.0fx%.0f world\n",
               cols, rows, WORLD_W, WORLD_H);
        assert(cols == 19 && rows == 13);   // ceil(9600/512), ceil(6400/512)

        spatial_grid_destroy(grid);
    }

    printf("\nTEST 2: inside the radius is found, outside is not\n");
    {
        SpatialGrid* grid = spatial_grid_create(WORLD_W, WORLD_H, CELL, MAX_ENT);
        SpatialPoint points[3] = {
            { 1000.0f, 1000.0f },   // 0 — right on the query point
            { 1000.0f, 1700.0f },   // 1 — 700 away, inside 800
            { 1000.0f, 1900.0f },   // 2 — 900 away, outside 800
        };
        spatial_grid_build(grid, points, 3);
        assert(spatial_grid_count(grid) == 3);

        int n = spatial_grid_query(grid, 1000.0f, 1000.0f, 800.0f, out, MAX_ENT);
        printf("  found %d (expect 2)\n", n);
        assert(n == 2);
        assert(out[0] == 0 && out[1] == 1);

        spatial_grid_destroy(grid);
    }

    printf("\nTEST 3: neighbours across a cell boundary are still found\n");
    {
        // straddle one cell boundary
        SpatialGrid* grid = spatial_grid_create(WORLD_W, WORLD_H, CELL, MAX_ENT);
        SpatialPoint points[2] = {
            { 511.0f, 300.0f },
            { 513.0f, 300.0f },
        };
        spatial_grid_build(grid, points, 2);

        int n = spatial_grid_query(grid, 511.0f, 300.0f, 100.0f, out, MAX_ENT);
        printf("  found %d across the seam (expect 2)\n", n);
        assert(n == 2);

        spatial_grid_destroy(grid);
    }

    printf("\nTEST 4: results come back nearest-first\n");
    {
        SpatialGrid* grid = spatial_grid_create(WORLD_W, WORLD_H, CELL, MAX_ENT);
        SpatialPoint points[5] = {
            { 5000.0f, 3000.0f },   // 0 — 700 away
            { 4400.0f, 3000.0f },   // 1 — 100 away  <- nearest
            { 4000.0f, 3000.0f },   // 2 — 300 away
            { 4800.0f, 3000.0f },   // 3 — 500 away
            { 3700.0f, 3000.0f },   // 4 — 600 away
        };
        spatial_grid_build(grid, points, 5);

        int n = spatial_grid_query(grid, 4300.0f, 3000.0f, 800.0f, out, MAX_ENT);
        assert(n == 5);
        printf("  order: %d %d %d %d %d (expect 1 2 3 4 0)\n",
               out[0], out[1], out[2], out[3], out[4]);
        assert(out[0] == 1 && out[1] == 2 && out[2] == 3 &&
               out[3] == 4 && out[4] == 0);

        spatial_grid_destroy(grid);
    }

    printf("\nTEST 5: a capped query returns the NEAREST n, not the first n\n");
    {
        // reverse array order relative to distance order
        SpatialGrid* grid = spatial_grid_create(WORLD_W, WORLD_H, CELL, MAX_ENT);

        SpatialPoint points[100];
        for (int i = 0; i < 100; i++) {
            points[i].x = 5000.0f + (float)(100 - i) * 5.0f;   // 500 .. 5 away
            points[i].y = 3000.0f;
        }
        spatial_grid_build(grid, points, 100);

        int n = spatial_grid_query(grid, 5000.0f, 3000.0f, 800.0f, out, 32);
        printf("  returned %d of 100 in range (expect 32)\n", n);
        assert(n == 32);

        // The nearest 32 are indices 99 down to 68, in that order.
        for (int i = 0; i < 32; i++) {
            assert(out[i] == 99 - i);
        }
        printf("  nearest is index %d at %.0fpx, farthest returned is %d at %.0fpx\n",
               out[0],  sqrtf(dist_sq_to(points, out[0],  5000.0f, 3000.0f)),
               out[31], sqrtf(dist_sq_to(points, out[31], 5000.0f, 3000.0f)));
        printf("  -> the old slot-order scan would have returned indices 0..31,\n");
        printf("     the 32 FARTHEST entities in range\n");

        spatial_grid_destroy(grid);
    }

    printf("\nTEST 6: a non-finite position is excluded, not filed at the origin\n");
    {
        SpatialGrid* grid = spatial_grid_create(WORLD_W, WORLD_H, CELL, MAX_ENT);
        SpatialPoint points[4] = {
            { 100.0f, 100.0f },
            { NAN,    100.0f },
            { 100.0f, INFINITY },
            { 150.0f, 100.0f },
        };
        spatial_grid_build(grid, points, 4);

        printf("  filed %d of 4 (expect 2)\n", spatial_grid_count(grid));
        assert(spatial_grid_count(grid) == 2);

        int n = spatial_grid_query(grid, 100.0f, 100.0f, 800.0f, out, MAX_ENT);
        assert(n == 2);
        assert(out[0] == 0 && out[1] == 3);

        // A garbage query is rejected rather than answered.
        assert(spatial_grid_query(grid, NAN, 100.0f, 800.0f, out, MAX_ENT) == 0);
        assert(spatial_grid_query(grid, 100.0f, 100.0f, NAN, out, MAX_ENT) == 0);
        assert(spatial_grid_query(grid, 100.0f, 100.0f, -1.0f, out, MAX_ENT) == 0);
        printf("  non-finite and negative queries all returned 0\n");

        spatial_grid_destroy(grid);
    }

    printf("\nTEST 7: positions outside the world extent still work\n");
    {
        // clamp positions beyond the configured extent
        SpatialGrid* grid = spatial_grid_create(480.0f, 496.0f, CELL, MAX_ENT);
        int cols = 0, rows = 0;
        spatial_grid_dimensions(grid, &cols, &rows);
        printf("  tiny world gives a %dx%d grid (expect 1x1)\n", cols, rows);
        assert(cols == 1 && rows == 1);

        SpatialPoint points[3] = {
            { 100.0f,   100.0f   },
            { 50000.0f, 50000.0f },   // far outside
            { -9000.0f, -9000.0f },   // negative
        };
        spatial_grid_build(grid, points, 3);
        assert(spatial_grid_count(grid) == 3);

        int n = spatial_grid_query(grid, 50000.0f, 50000.0f, 10.0f, out, MAX_ENT);
        printf("  found %d at the far-outside position (expect 1)\n", n);
        assert(n == 1 && out[0] == 1);

        n = spatial_grid_query(grid, 100.0f, 100.0f, 800.0f, out, MAX_ENT);
        printf("  found %d near the origin (expect 1 — the others are far away)\n", n);
        assert(n == 1 && out[0] == 0);

        spatial_grid_destroy(grid);
    }

    printf("\nTEST 8: rebuilding replaces the previous contents\n");
    {
        SpatialGrid* grid = spatial_grid_create(WORLD_W, WORLD_H, CELL, MAX_ENT);

        SpatialPoint first[2] = { { 100.0f, 100.0f }, { 200.0f, 200.0f } };
        spatial_grid_build(grid, first, 2);
        assert(spatial_grid_query(grid, 100.0f, 100.0f, 800.0f, out, MAX_ENT) == 2);

        SpatialPoint second[1] = { { 8000.0f, 5000.0f } };
        spatial_grid_build(grid, second, 1);
        assert(spatial_grid_count(grid) == 1);

        int n = spatial_grid_query(grid, 100.0f, 100.0f, 800.0f, out, MAX_ENT);
        printf("  stale entities left after rebuild: %d (expect 0)\n", n);
        assert(n == 0);
        assert(spatial_grid_query(grid, 8000.0f, 5000.0f, 800.0f, out, MAX_ENT) == 1);

        spatial_grid_destroy(grid);
    }

    printf("\nTEST 9: an over-long build is clamped, not a buffer overrun\n");
    {
        SpatialGrid* grid = spatial_grid_create(WORLD_W, WORLD_H, CELL, 10);
        SpatialPoint points[50];
        for (int i = 0; i < 50; i++) {
            points[i].x = 1000.0f + (float)i;
            points[i].y = 1000.0f;
        }
        spatial_grid_build(grid, points, 50);
        printf("  filed %d of 50 into a grid sized for 10 (expect 10)\n",
               spatial_grid_count(grid));
        assert(spatial_grid_count(grid) == 10);

        spatial_grid_destroy(grid);
    }

    printf("\nTEST 10: 1000 random entities, 500 random queries vs brute force\n");
    {
        SpatialGrid* grid = spatial_grid_create(WORLD_W, WORLD_H, CELL, MAX_ENT);

        static SpatialPoint points[MAX_ENT];
        for (int i = 0; i < MAX_ENT; i++) {
            points[i].x = rand_range(-500.0f, WORLD_W + 500.0f);
            points[i].y = rand_range(-500.0f, WORLD_H + 500.0f);
        }
        spatial_grid_build(grid, points, MAX_ENT);

        int total_found = 0;
        for (int q = 0; q < 500; q++) {
            float qx = rand_range(0.0f, WORLD_W);
            float qy = rand_range(0.0f, WORLD_H);
            float radius = rand_range(50.0f, 2500.0f);

            int n_grid  = spatial_grid_query(grid, qx, qy, radius, out, MAX_ENT);
            int n_brute = brute_force(points, MAX_ENT, qx, qy, radius, expect, MAX_ENT);

            assert(n_grid == n_brute);
            for (int i = 0; i < n_grid; i++) assert(out[i] == expect[i]);
            total_found += n_grid;
        }
        printf("  500 uncapped queries matched brute force exactly (%d hits total)\n",
               total_found);

        // compare capped nearest-first results
        for (int q = 0; q < 500; q++) {
            float qx = rand_range(0.0f, WORLD_W);
            float qy = rand_range(0.0f, WORLD_H);

            int n_grid  = spatial_grid_query(grid, qx, qy, 800.0f, out, 32);
            int n_brute = brute_force(points, MAX_ENT, qx, qy, 800.0f, expect, 32);

            assert(n_grid == n_brute);
            for (int i = 0; i < n_grid; i++) assert(out[i] == expect[i]);
        }
        printf("  500 capped (32) queries matched brute force exactly\n");

        // Ordering must hold on its own terms, not just against the oracle.
        int n = spatial_grid_query(grid, 4000.0f, 3000.0f, 2000.0f, out, MAX_ENT);
        for (int i = 1; i < n; i++) {
            assert(dist_sq_to(points, out[i - 1], 4000.0f, 3000.0f) <=
                   dist_sq_to(points, out[i],     4000.0f, 3000.0f));
        }
        printf("  a %d-result query is monotonically ordered by distance\n", n);

        spatial_grid_destroy(grid);
    }

    printf("\nTEST 11: a crowd stacked in one cell degrades correctly\n");
    {
        // test the single-cell crowd worst case
        SpatialGrid* grid = spatial_grid_create(WORLD_W, WORLD_H, CELL, MAX_ENT);

        static SpatialPoint points[MAX_ENT];
        for (int i = 0; i < MAX_ENT; i++) {
            points[i].x = 3000.0f + rand_range(-20.0f, 20.0f);
            points[i].y = 3000.0f + rand_range(-20.0f, 20.0f);
        }
        spatial_grid_build(grid, points, MAX_ENT);

        int n_grid  = spatial_grid_query(grid, 3000.0f, 3000.0f, 800.0f, out, 32);
        int n_brute = brute_force(points, MAX_ENT, 3000.0f, 3000.0f, 800.0f, expect, 32);
        assert(n_grid == 32 && n_brute == 32);
        for (int i = 0; i < 32; i++) assert(out[i] == expect[i]);
        printf("  1000 entities in one cell: nearest 32 still exact\n");

        spatial_grid_destroy(grid);
    }

    printf("\nTEST 12: bad arguments are refused, not crashed on\n");
    {
        assert(spatial_grid_create(WORLD_W, WORLD_H, 0.0f, MAX_ENT) == NULL);
        assert(spatial_grid_create(WORLD_W, WORLD_H, -1.0f, MAX_ENT) == NULL);
        assert(spatial_grid_create(WORLD_W, WORLD_H, CELL, 0) == NULL);
        assert(spatial_grid_create(NAN, WORLD_H, CELL, MAX_ENT) == NULL);

        SpatialGrid* grid = spatial_grid_create(WORLD_W, WORLD_H, CELL, MAX_ENT);
        SpatialPoint p = { 100.0f, 100.0f };
        spatial_grid_build(grid, &p, 1);

        assert(spatial_grid_query(grid, 100.0f, 100.0f, 800.0f, out, 0) == 0);
        assert(spatial_grid_query(grid, 100.0f, 100.0f, 800.0f, NULL, 10) == 0);
        assert(spatial_grid_query(NULL, 100.0f, 100.0f, 800.0f, out, 10) == 0);
        assert(spatial_grid_count(NULL) == 0);

        spatial_grid_build(grid, NULL, 5);      // must not crash
        assert(spatial_grid_count(grid) == 0);

        spatial_grid_destroy(grid);
        spatial_grid_destroy(NULL);             // must not crash
        printf("  all invalid arguments handled\n");
    }

    printf("\nALL ASSERTIONS PASSED\n");
    return 0;
}
