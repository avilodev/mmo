#ifndef WORLD_COLLISION_H
#define WORLD_COLLISION_H

// Load the collision layer from world.dat.
// Returns 1 on success, 0 on failure (server runs without collision if file missing).
int world_collision_init(const char* path);

// Returns 1 if the tile at pixel position (x, y) is solid.
int world_collision_check(float x, float y);

// Returns 1 if any corner of the axis-aligned box centered at (x, y)
// with the given half_size is solid.  Mirrors client world_check_box_collision.
int world_collision_check_box(float x, float y, float half_size);

void world_collision_shutdown(void);

#endif // WORLD_COLLISION_H
