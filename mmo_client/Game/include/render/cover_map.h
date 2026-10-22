#ifndef COVER_MAP_H
#define COVER_MAP_H

// The city seen from straight above, as the height of the highest surface over
// every spot (the depth of a top-down drawing). Rain and snow stop at that
// height: the ground in the open, a roof or an awning over it. Drawn once when
// the city is loaded, and read by the weather shaders. A coarse copy goes to
// world/cover_field.h for the sound and the other code that has no GPU.

#include "render/gl_loader.h"

// The texture unit the map is sampled from
#define COVER_MAP_UNIT 4

typedef struct {
	GLint texture, origin, scale, top;
} cover_uniforms;

// Draw the map from the loaded city (city_renderer_init() does it). Needs
// shadow_map_init(). Returns nonzero on success; without it nothing is covered.
int cover_map_build(void);

void cover_map_shutdown(void);

int cover_map_ready(void);

// GLSL for a shader to paste in: the uniforms and
//     float cover_height(vec2 world_xz)
// the height of the highest surface there, or a huge negative one where the
// map does not reach. Use textureLod, so a vertex shader can use it too.
const char* cover_map_glsl(void);

// Find the uniforms in a linked program
void cover_map_locate(GLuint program, cover_uniforms* out);

// Bind the map to its unit and set the uniforms of the program in use
void cover_map_apply(const cover_uniforms* u);

#endif /* COVER_MAP_H */
