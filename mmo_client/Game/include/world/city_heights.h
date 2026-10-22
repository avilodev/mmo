#ifndef CITY_HEIGHTS_H
#define CITY_HEIGHTS_H

/**
 * @file
 * How tall the city's solid tiles are, for the camera.
 *
 * Game/tools/export_city_collision.py bakes this beside the collision it writes
 * into world.dat. Collision is flat (a tile is solid or not); the camera also
 * needs to know a wall from a fence, so it can stay in front of one and look
 * over the other. Visual only: nothing here is sent to the server.
 */

#include "render/city_renderer.h"

#define CITY_HEIGHTS_PATH "Game/assets/baked/test_town_heights.bin"

/** Load the heights. @return 1 on success; 0, logged, if the file is missing
 *  or malformed (the camera then ignores walls). */
int  city_heights_load(const char* path);
void city_heights_free(void);

/** Height in world units of the solid at a world point, or 0 where nothing is
 *  solid. Matches CameraHeightFn; `ctx` is unused. */
float city_heights_at(void* ctx, float world_x, float world_y);

#endif /* CITY_HEIGHTS_H */
