#ifndef SHADOW_PASS_H
#define SHADOW_PASS_H

// Draw the sun's (or the moon's) shadows for a frame: the city and the bodies
// seen last frame, from the light, into the shadow maps.

#include "render/shadow_map.h"
#include "world/shadow_math.h"
#include "world/world_lighting.h"

// Draw the maps round a point. Call once a frame before the world is drawn;
// leaves the maps empty (and shadow_pass_active() zero) when there is no
// shadow to cast or no map to cast it on.
void shadow_pass_run(const world_lighting* light, float focus_x, float focus_y, float focus_h);

// Whether this frame's maps hold shadows
int shadow_pass_active(void);

// The matrices and sizes of a map drawn this frame
const shadow_cascade* shadow_pass_cascade(int cascade);

#endif /* SHADOW_PASS_H */
