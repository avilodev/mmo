#ifndef SCENE_LIGHT_H
#define SCENE_LIGHT_H

// The lighting every lit shader shares: one function that gives the light a
// surface receives (the sun or moon, the sky and ground ambient, shadows, and
// the lamps), and the uniforms it reads. A shader pastes scene_light_glsl()
// into its fragment source and calls
//     vec3 scene_light(vec3 normal, vec3 world_position)
// and a renderer fills the uniforms with scene_light_apply().
//
// The weather is in there too: scene_surface(albedo, normal, world_position)
// gives wet and snowy ground its look, and scene_fog(colour, world_position)
// thickens the air with distance from the eye (scene_light_set_eye()), and
// leaves ground under a roof dry. The roofs are the cover map's (cover_map.h).

#include "render/gl_loader.h"
#include "render/cover_map.h"
#include "render/weather_map.h"
#include "render/lamp_shadow.h"
#include "render/shadow_map.h"

#define SCENE_LIGHT_MAX_LAMPS LAMP_SHADOW_MAX_LAMPS

typedef struct {
	GLint light_dir, light_color, ambient_up, ambient_down;
	GLint shadow_strength, shadow_bias, shadow_offset;
	GLint shadow_map[SHADOW_CASCADES];
	GLint shadow_mat[SHADOW_CASCADES];
	GLint lamp_count, lamp_shadow, lamp_shadowed;
	GLint lamp_pos[SCENE_LIGHT_MAX_LAMPS];
	GLint lamp_radius[SCENE_LIGHT_MAX_LAMPS];
	GLint lamp_color[SCENE_LIGHT_MAX_LAMPS];
	GLint eye, fog_color, fog_density, weather, weather_time, cloud_drift;
	cover_uniforms cover;
	weather_map_uniforms map;
} scene_light_uniforms;

// GLSL: the uniforms and scene_light(). Goes after the shader's own inputs
// and before its main().
const char* scene_light_glsl(void);

// Find the uniforms in a linked program
void scene_light_locate(GLuint program, scene_light_uniforms* out);

// Set the uniforms for the program in use. The lamps are this frame's, from
// lamp_shadow_run(). The
// sun's shadow maps go on texture units 1 and 2 and the lamps' on unit 3;
// unit 0 is active again afterwards.
void scene_light_apply(const scene_light_uniforms* u);

// Where the camera's eye is this frame, for the fog and the wet gleam
void scene_light_set_eye(const float eye[3]);

// Light with plain daylight, no shadows, no lamps and no weather, whatever the
// hour: for the map and the previews, which show things as they are.
void scene_light_set_flat(int flat);

#endif /* SCENE_LIGHT_H */
