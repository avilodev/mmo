#ifndef CITY_RENDERER_H
#define CITY_RENDERER_H

/**
 * @file
 * Draw the authored city scene (a Blender OBJ export) as static, textured,
 * sun-lit geometry in the world.
 *
 * Visual only: the server still walks players against world.dat's collision,
 * which knows nothing of this scene.
 */

#include "camera/camera_math.h"
#include "render/character_tuning.h"
#include "world_regions.h"

#define CITY_OBJ_PATH   "Game/assets/models/all_assets/example_city.obj"
/** Binary copy of the parsed OBJ, rebuilt whenever the OBJ changes. */
#define CITY_CACHE_PATH "Game/assets/models/all_assets/example_city.objcache"

/** The scene is in metres (a bench is 1 m, a street lamp 3.7 m); this is the
 *  same scale the character model is drawn at (character_tuning.h). */
#define CITY_UNITS_PER_METRE (CHARACTER_HEIGHT / 1.83f)

/** World position of the scene's origin: Ennara's courtyard centre, where
 *  new characters wake up. Blender +X is east and -Y (OBJ +Z) is south. */
#define CITY_ORIGIN_X ((float)CITY_ENNARA_TILE_X * WORLD_TILE_PX)
#define CITY_ORIGIN_Y ((float)CITY_ENNARA_TILE_Y * WORLD_TILE_PX)

/** Side of the ground cells the scene is split into for culling, in world
 *  units: one world chunk. */
#define CITY_CELL_UNITS 512.0f

/** Load the scene and put it on the GPU. Needs gl_loader_init().
 *  @return Nonzero on success; a missing or broken file is logged. */
int  city_renderer_init(void);
void city_renderer_shutdown(void);

/** Draw the cells in view and inside the fog around (focus_x, focus_y), with
 *  depth writes on so what stands in front of them later is sorted by depth. */
void city_renderer_draw(const CameraView* view, float focus_x, float focus_y);

#endif /* CITY_RENDERER_H */
