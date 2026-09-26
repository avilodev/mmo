#ifndef SPATIAL_GRID_H
#define SPATIAL_GRID_H

/**
 * @file
 * Declare a caller-indexed uniform grid for world proximity queries.
 * Each grid owns query scratch storage and must remain confined to one thread.
 */

#include <stddef.h>

/** Default cell edge length in world pixels. */
#define SPATIAL_GRID_DEFAULT_CELL 512.0f

/** Store one entity position in world coordinates. */
typedef struct {
    float x, y;
} SpatialPoint;

typedef struct SpatialGrid SpatialGrid;

SpatialGrid* spatial_grid_create(float world_width, float world_height,
                                 float cell_size, int max_entities);

void spatial_grid_destroy(SpatialGrid* grid);

void spatial_grid_build(SpatialGrid* grid, const SpatialPoint* points, int count);

int spatial_grid_query(SpatialGrid* grid, float x, float y, float radius,
                       int* out_indices, int max_out);

/** Bytes of scratch spatial_grid_query_into() needs for this grid.
 *
 * @return The required size, or 0 for a NULL grid.
 */
size_t spatial_grid_scratch_bytes(const SpatialGrid* grid);

/** Query a grid using caller-owned scratch, leaving the grid untouched.
 *
 * spatial_grid_query() sorts candidates inside storage the grid owns, which is
 * why a grid is confined to one thread. This variant writes into scratch the
 * caller supplies, so a grid that nothing is currently building may be queried
 * from several threads at once -- which is what lets a packet-handling thread
 * use the same index the gameplay tick built instead of scanning the pool.
 *
 * @param scratch        Buffer of at least spatial_grid_scratch_bytes().
 * @param scratch_bytes  Its size; a smaller buffer yields no results.
 * @return               Number of indices written.
 */
int spatial_grid_query_into(const SpatialGrid* grid, float x, float y, float radius,
                            int* out_indices, int max_out,
                            void* scratch, size_t scratch_bytes);

int spatial_grid_count(const SpatialGrid* grid);

void spatial_grid_dimensions(const SpatialGrid* grid, int* out_cols, int* out_rows);

#endif // SPATIAL_GRID_H
