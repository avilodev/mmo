#ifndef WORLD_COLLISION_H
#define WORLD_COLLISION_H

// Load the collision layer from world.dat.
// Returns 1 on success, 0 on failure.
//
// Every query below fails closed when no map is loaded: with no map there is
// no way to tell open ground from a wall, and answering "open" would silently
// turn off movement validation for the whole world. The world server treats a
// failed load as fatal for the same reason.
int world_collision_init(const char* path);

// Returns 1 once a collision map is loaded.
int world_collision_is_loaded(void);

// World extent in pixels. Both outputs are set to 0 when no map is loaded.
void world_collision_extent(float* out_width, float* out_height);

// Returns 1 when (x, y) is finite and inside the loaded world extent, i.e.
// when it is safe to do arithmetic with. Non-finite input is never valid.
int world_coord_is_valid(float x, float y);

// Returns 1 if the tile at pixel position (x, y) is solid. Non-finite and
// out-of-bounds positions count as solid.
int world_collision_check(float x, float y);

// Returns 1 if any corner of the axis-aligned box centered at (x, y)
// with the given half_size is solid.  Mirrors client world_check_box_collision.
int world_collision_check_box(float x, float y, float half_size);

// Returns 1 if the box hits a solid tile anywhere along the segment from
// (x0, y0) to (x1, y1). Testing only the endpoint lets a single long step
// straddle a wall — the box starts clear, ends clear, and passes straight
// through the solid tiles in between. Samples at half-tile intervals so no
// solid tile can fall between two samples.
//
// The origin itself is not tested: the player is already standing there, and
// a player who ends up inside geometry must still be able to walk out of it.
int world_collision_check_box_path(float x0, float y0,
                                   float x1, float y1,
                                   float half_size);

void world_collision_shutdown(void);

#endif // WORLD_COLLISION_H
