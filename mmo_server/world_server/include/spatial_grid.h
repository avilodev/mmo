#ifndef SPATIAL_GRID_H
#define SPATIAL_GRID_H

/**
 * @file
 * Declare a caller-indexed uniform grid for world proximity queries.
 * Each grid owns query scratch storage and must remain confined to one thread.
 */

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

int spatial_grid_count(const SpatialGrid* grid);

void spatial_grid_dimensions(const SpatialGrid* grid, int* out_cols, int* out_rows);

#endif // SPATIAL_GRID_H
