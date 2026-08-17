#ifndef WORLD_COLLISION_H
#define WORLD_COLLISION_H

/** @file Query the collision layer loaded from world.dat.
 * Queries fail closed before initialization or outside finite world bounds.
 */

// return nonzero after loading a valid collision layer
int world_collision_init(const char* path);

int world_collision_is_loaded(void);

// report pixel extent or zero both outputs before initialization
void world_collision_extent(float* out_width, float* out_height);

// return whether coordinates are finite and inside the world extent
int world_coord_is_valid(float x, float y);

// treat non-finite and out-of-bounds pixel positions as solid
int world_collision_check(float x, float y);

// test every corner of an axis-aligned box against solid tiles
int world_collision_check_box(float x, float y, float half_size);

// sample at half-tile intervals while excluding the path origin
int world_collision_check_box_path(float x0, float y0,
                                   float x1, float y1,
                                   float half_size);

void world_collision_shutdown(void);

#endif // WORLD_COLLISION_H
