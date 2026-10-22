#ifndef WEATHER_MAP_H
#define WEATHER_MAP_H

// The patches of weather round the camera (world/weather_cells.h) as a small
// texture, so the shaders can ask what the weather is at any spot: the rain
// only falls where a patch is, the sky is cloudier over one, and the air is
// thick under it. Cloud, rain, snow and storm are the red, green, blue and
// alpha. Rebuilt a few times a second from the weather of the moment.

#include "render/gl_loader.h"

// The texture unit the map is sampled from
#define WEATHER_MAP_UNIT 5

// How far across the map reaches, in world units: far enough to see a patch coming
#define WEATHER_MAP_SPAN 12000.0f

typedef struct {
	GLint texture, origin, scale, global;
} weather_map_uniforms;

// Make the texture. Returns nonzero on success; without it the shaders use the
// weather at the player everywhere.
int weather_map_init(void);

void weather_map_shutdown(void);

// Rebuild the map round a world position when it is due. Call once a frame.
void weather_map_update(float focus_x, float focus_y);

// The most of each there is anywhere on the map, 0 to 1, so a fall is only drawn
// when there is one to draw
float weather_map_peak_rain(void);
float weather_map_peak_snow(void);

// GLSL for a shader to paste in: the uniforms and
//     vec4 weather_at(vec2 world_xz)   // cloud, rain, snow, storm
// Use it from a vertex shader too.
const char* weather_map_glsl(void);

// Find the uniforms in a linked program
void weather_map_locate(GLuint program, weather_map_uniforms* out);

// Bind the map to its unit and set the uniforms of the program in use. When off is
// nonzero the weather is none anywhere (a map or a preview).
void weather_map_apply(const weather_map_uniforms* u, int off);

#endif /* WEATHER_MAP_H */
