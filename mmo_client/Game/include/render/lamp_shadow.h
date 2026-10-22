#ifndef LAMP_SHADOW_H
#define LAMP_SHADOW_H

// The shadows of the street lamps. Each lamp near the camera is drawn from six
// views (world/lamp_shadow_math.h) into one atlas texture: a row of six square
// tiles per lamp. Done only at night, when the lamps are on.

#include "world/city_lamps.h"

#define LAMP_SHADOW_MAX_LAMPS 8
#define LAMP_SHADOW_TILE 512
// Lamps this far from the camera's centre shine; farther ones are off
#define LAMP_SHADOW_RANGE 1500.0f
// Nothing closer than this to a lamp casts or receives its shadow, so the
// lamp's own housing, which the light sits inside, does not shade everything
#define LAMP_SHADOW_NEAR 12.0f

// Make the atlas. Needs shadow_map_init() first. Returns nonzero on success;
// failures are logged, and the lamps then glow without casting shadows.
int lamp_shadow_init(void);
void lamp_shadow_shutdown(void);

// Choose this frame's lamps, the ones nearest the camera's centre, and draw
// their shadows. `lamp_amount` is how far the lamps are on, 0 to 1.
void lamp_shadow_run(float lamp_amount, float focus_x, float focus_y);

// This frame's lamps, nearest first. Returns how many.
int lamp_shadow_lamps(const city_lamp** out);

// Whether this frame's lamps have shadows drawn
int lamp_shadow_active(void);

// The atlas texture, to sample from
unsigned int lamp_shadow_texture(void);

#endif /* LAMP_SHADOW_H */
