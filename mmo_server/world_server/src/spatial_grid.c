/**
 * @file
 * Index world entities in uniform cells for bounded proximity queries.
 */

#include "spatial_grid.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/** Describe a proximity candidate before result selection. */
typedef struct {
    int   index;
    float dist_sq;
} Candidate;

struct SpatialGrid {
    float cell_size;
    float inv_cell;        /**< Reciprocal cell size. */

    int   cols, rows;
    int   cell_count;
    int   max_entities;
    int   count;           // entities filed by the last build

    int*        cell_start;   /**< Prefix-sum offsets for cell runs. */
    int*        cursor;       /**< Per-cell write positions used during builds. */
    int*        entry_index;  /**< Caller indices grouped by cell. */
    SpatialPoint* entry_pos;  /**< Positions parallel to entry_index. */
    Candidate*  scratch;      /**< Query candidates owned by the grid. */
};

static int column_of(const SpatialGrid* grid, float x) {
    float fx = x * grid->inv_cell;
    if (!(fx > 0.0f)) return 0;                        // also catches NaN
    if (fx >= (float)grid->cols) return grid->cols - 1;
    return (int)fx;
}

static int row_of(const SpatialGrid* grid, float y) {
    float fy = y * grid->inv_cell;
    if (!(fy > 0.0f)) return 0;
    if (fy >= (float)grid->rows) return grid->rows - 1;
    return (int)fy;
}

static int cell_of(const SpatialGrid* grid, float x, float y) {
    return row_of(grid, y) * grid->cols + column_of(grid, x);
}

static int point_is_usable(const SpatialPoint* point) {
    return isfinite(point->x) && isfinite(point->y);
}

/** Order candidates by distance and then caller index. */
static int candidate_compare(const void* a, const void* b) {
    const Candidate* ca = a;
    const Candidate* cb = b;
    if (ca->dist_sq < cb->dist_sq) return -1;
    if (ca->dist_sq > cb->dist_sq) return 1;
    if (ca->index < cb->index) return -1;
    if (ca->index > cb->index) return 1;
    return 0;
}

static inline int candidate_before(const Candidate* a, const Candidate* b) {
    if (a->dist_sq != b->dist_sq) return a->dist_sq < b->dist_sq;
    return a->index < b->index;
}

/** Retain the nearest ordered candidates at the front of the scratch array. */
static void select_nearest(Candidate* scratch, int count, int keep) {
    if (keep >= count) return;

    // seed the bounded ordered window
    for (int i = 1; i < keep; i++) {
        Candidate c = scratch[i];
        int j = i - 1;
        while (j >= 0 && candidate_before(&c, &scratch[j])) {
            scratch[j + 1] = scratch[j];
            j--;
        }
        scratch[j + 1] = c;
    }

    for (int i = keep; i < count; i++) {
        if (!candidate_before(&scratch[i], &scratch[keep - 1])) continue;

        Candidate c = scratch[i];
        int j = keep - 2;
        while (j >= 0 && candidate_before(&c, &scratch[j])) {
            scratch[j + 1] = scratch[j];
            j--;
        }
        scratch[j + 1] = c;
    }
}

/**
 * Create a uniform spatial grid with fixed storage capacity.
 *
 * The caller owns the returned grid and must destroy it with spatial_grid_destroy().
 *
 * @param world_width  Horizontal grid extent in world units.
 * @param world_height  Vertical grid extent in world units.
 * @param cell_size  Positive cell edge length in world units.
 * @param max_entities  Positive maximum number of indexed entities.
 * @return      A new grid, or NULL for invalid dimensions or allocation failure.
 */
SpatialGrid* spatial_grid_create(float world_width, float world_height,
                                 float cell_size, int max_entities) {
    if (!isfinite(world_width) || !isfinite(world_height) ||
        !isfinite(cell_size) || cell_size <= 0.0f || max_entities <= 0) {
        return NULL;
    }

    SpatialGrid* grid = calloc(1, sizeof(*grid));
    if (!grid) return NULL;

    grid->cell_size = cell_size;
    grid->inv_cell  = 1.0f / cell_size;

    // preserve one cell for sub-cell extents
    float cols = ceilf(world_width  / cell_size);
    float rows = ceilf(world_height / cell_size);
    grid->cols = (cols >= 1.0f) ? (int)cols : 1;
    grid->rows = (rows >= 1.0f) ? (int)rows : 1;

    if (grid->cols > (1 << 20) || grid->rows > (1 << 20)) {
        free(grid);
        return NULL;
    }

    grid->cell_count   = grid->cols * grid->rows;
    grid->max_entities = max_entities;

    grid->cell_start  = calloc((size_t)grid->cell_count + 1, sizeof(int));
    grid->cursor      = calloc((size_t)grid->cell_count, sizeof(int));
    grid->entry_index = calloc((size_t)max_entities, sizeof(int));
    grid->entry_pos   = calloc((size_t)max_entities, sizeof(SpatialPoint));
    grid->scratch     = calloc((size_t)max_entities, sizeof(Candidate));

    if (!grid->cell_start || !grid->cursor || !grid->entry_index ||
        !grid->entry_pos || !grid->scratch) {
        spatial_grid_destroy(grid);
        return NULL;
    }

    return grid;
}

/**
 * Destroy a spatial grid and its internal storage.
 *
 * @param grid  Grid to destroy, or NULL.
 */
void spatial_grid_destroy(SpatialGrid* grid) {
    if (!grid) return;
    free(grid->cell_start);
    free(grid->cursor);
    free(grid->entry_index);
    free(grid->entry_pos);
    free(grid->scratch);
    free(grid);
}

/**
 * Rebuild a grid from caller-indexed entity positions.
 *
 * Non-finite points are excluded and counts above the configured capacity are truncated.
 *
 * @param grid  Grid to replace; may be NULL.
 * @param points  Position array whose indices identify entities, or NULL to clear the grid.
 * @param count  Number of positions available.
 */
void spatial_grid_build(SpatialGrid* grid, const SpatialPoint* points, int count) {
    if (!grid) return;

    grid->count = 0;
    memset(grid->cell_start, 0, ((size_t)grid->cell_count + 1) * sizeof(int));

    if (!points || count <= 0) return;
    if (count > grid->max_entities) count = grid->max_entities;

    // tally into shifted prefix-sum slots
    for (int i = 0; i < count; i++) {
        if (!point_is_usable(&points[i])) continue;
        grid->cell_start[cell_of(grid, points[i].x, points[i].y) + 1]++;
        grid->count++;
    }

    // convert counts to cell-run offsets
    for (int c = 0; c < grid->cell_count; c++) {
        grid->cell_start[c + 1] += grid->cell_start[c];
    }
    memcpy(grid->cursor, grid->cell_start, (size_t)grid->cell_count * sizeof(int));

    // place entries into contiguous cell runs
    for (int i = 0; i < count; i++) {
        if (!point_is_usable(&points[i])) continue;
        int slot = grid->cursor[cell_of(grid, points[i].x, points[i].y)]++;
        grid->entry_index[slot] = i;
        grid->entry_pos[slot]   = points[i];
    }
}

/**
 * Write nearest entity indices within a circular search area.
 *
 * Results are ordered nearest-first with caller indices breaking distance ties.
 *
 * @param grid  Grid to query; its own scratch storage is modified, so the grid
 *              must not be queried concurrently. Use spatial_grid_query_into()
 *              with caller-owned scratch when it must be.
 * @param x  Search center x-coordinate in world units.
 * @param y  Search center y-coordinate in world units.
 * @param radius  Non-negative search radius in world units.
 * @param out_indices  Buffer receiving caller-side entity indices.
 * @param max_out  Positive capacity of out_indices.
 * @return      Number of indices written, or 0 for invalid input or no matches.
 */
int spatial_grid_query(SpatialGrid* grid, float x, float y, float radius,
                       int* out_indices, int max_out) {
    if (!grid) return 0;
    return spatial_grid_query_into(grid, x, y, radius, out_indices, max_out,
                                   grid->scratch,
                                   (size_t)grid->max_entities * sizeof(Candidate));
}

/**
 * Bytes of scratch spatial_grid_query_into() needs for this grid.
 */
size_t spatial_grid_scratch_bytes(const SpatialGrid* grid) {
    if (!grid) return 0;
    return (size_t)grid->max_entities * sizeof(Candidate);
}

/**
 * Query a grid using caller-owned scratch, leaving the grid untouched.
 *
 * Results are ordered nearest-first with caller indices breaking distance ties.
 *
 * @param grid  Grid to query; not modified.
 * @param x  Search center x-coordinate in world units.
 * @param y  Search center y-coordinate in world units.
 * @param radius  Non-negative search radius in world units.
 * @param out_indices  Buffer receiving caller-side entity indices.
 * @param max_out  Positive capacity of out_indices.
 * @param scratch  Buffer of at least spatial_grid_scratch_bytes().
 * @param scratch_bytes  Its size.
 * @return      Number of indices written, or 0 for invalid input or no matches.
 */
int spatial_grid_query_into(const SpatialGrid* grid, float x, float y, float radius,
                            int* out_indices, int max_out,
                            void* scratch, size_t scratch_bytes) {
    if (!grid || !out_indices || max_out <= 0 || !scratch) return 0;
    if (!isfinite(x) || !isfinite(y) || !isfinite(radius) || radius < 0.0f) return 0;
    if (grid->count == 0) return 0;

    /* A short buffer would be overrun by the candidate collection below, and
     * silently returning nothing is the failure a caller can actually notice. */
    if (scratch_bytes < (size_t)grid->max_entities * sizeof(Candidate)) return 0;

    Candidate* candidates = scratch;
    const float radius_sq = radius * radius;

    // restrict scans to the circle's cell bounds
    const int min_col = column_of(grid, x - radius);
    const int max_col = column_of(grid, x + radius);
    const int min_row = row_of(grid, y - radius);
    const int max_row = row_of(grid, y + radius);

    int found = 0;

    for (int row = min_row; row <= max_row; row++) {
        const int row_base = row * grid->cols;

        const int span_begin = grid->cell_start[row_base + min_col];
        const int span_end   = grid->cell_start[row_base + max_col + 1];

        for (int k = span_begin; k < span_end; k++) {
            const float dx = grid->entry_pos[k].x - x;
            const float dy = grid->entry_pos[k].y - y;
            const float dist_sq = dx * dx + dy * dy;
            if (dist_sq > radius_sq) continue;

            candidates[found].index   = grid->entry_index[k];
            candidates[found].dist_sq = dist_sq;
            found++;
        }
    }

    if (found == 0) return 0;

    const int written = (found < max_out) ? found : max_out;

    if (found <= max_out) {
        qsort(candidates, (size_t)found, sizeof(Candidate), candidate_compare);
    } else {
        // bound ordering work to the output capacity
        select_nearest(candidates, found, max_out);
    }

    for (int i = 0; i < written; i++) {
        out_indices[i] = candidates[i].index;
    }
    return written;
}

/**
 * Report the number of positions stored by the last build.
 *
 * @return      Stored position count, or 0 for a NULL grid.
 */
int spatial_grid_count(const SpatialGrid* grid) {
    return grid ? grid->count : 0;
}

/**
 * Write the grid dimensions in cells.
 *
 * @param grid  Grid to inspect, or NULL to report zero dimensions.
 * @param out_cols  Optional destination for the column count.
 * @param out_rows  Optional destination for the row count.
 */
void spatial_grid_dimensions(const SpatialGrid* grid, int* out_cols, int* out_rows) {
    if (out_cols) *out_cols = grid ? grid->cols : 0;
    if (out_rows) *out_rows = grid ? grid->rows : 0;
}
