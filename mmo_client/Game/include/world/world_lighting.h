#ifndef WORLD_LIGHTING_H
#define WORLD_LIGHTING_H

// How the world is lit at an hour of the day: the one big light (the sun by
// day, the moon by night), the ambient light from the sky above and the ground
// below, how strongly it casts shadows, and whether the lamps are on. No GL.
// Directions are GL world space: +X east, +Y up, +Z south. Colours are 0..1.
// At noon a surface facing straight up comes out at exactly 1.

#include "world/sky.h"

// The share of the big light left under solid cloud
#define WORLD_LIGHTING_OVERCAST_SUN 0.35

typedef struct {
	float dir[3];           // toward the light, unit length
	float color[3];         // the light's colour times its brightness
	float ambient_up[3];    // what a surface facing up gets from the sky
	float ambient_down[3];  // what a surface facing down gets from the ground
	float shadow_strength;  // 0 casts no shadow, 1 full shadows
	float lamp_amount;      // 0 lamps off, 1 lamps full on
} world_lighting;

// The lighting under a sky
void world_lighting_from_sky(const sky_state* sky, world_lighting* out);

// Plain daylight with no shadows and no lamps, for maps and previews
void world_lighting_noon(world_lighting* out);

// What a surface with this unit normal receives from the big light and the
// ambient light together, with nothing in the way. The shaders do the same sum.
void world_lighting_surface(const world_lighting* light, const float normal[3], float out[3]);

// Dim the big light under cloud, fade its shadows, and flash the ambient for
// lightning. Snow on the ground brightens the ambient a little.
void world_lighting_apply_weather(world_lighting* light, const weather_now* weather);

// Set the lighting the renderers draw with: the sky's, under this moment's weather
void world_lighting_update(const sky_state* sky);

// The lighting last given to world_lighting_update; noon until then
const world_lighting* world_lighting_current(void);

#endif /* WORLD_LIGHTING_H */
