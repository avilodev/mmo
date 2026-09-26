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

/** Report whether the collision layer is mapped rather than copied.
 *
 * The layer is read-only for the process's whole life and byte-identical
 * across every world on the host, so it is mapped shared: one physical copy
 * behind all of them, page-cache backed, and no per-process read at startup.
 * A filesystem that will not map it falls back to a private copy, which is
 * correct but is the thing worth knowing about on a host running ten worlds.
 * Setting MMO_COLLISION_NO_MMAP to anything but 0 forces that fallback, which
 * is how it is tested and how a deployment that has to avoid mapping says so.
 *
 * @return 1 when mapped, 0 when copied or not loaded.
 */
int world_collision_is_mapped(void);

void world_collision_shutdown(void);

#endif // WORLD_COLLISION_H
