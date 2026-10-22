// The top-down map of what is over every spot of the city.

#include "render/gl_loader.h"
#include "render/cover_map.h"
#include "render/city_renderer.h"
#include "render/shadow_map.h"
#include "world/cover_field.h"
#include "core/client_log.h"

#include <math.h>
#include <stdlib.h>

// Texels along a side of the map, and of the coarse copy the code keeps
#define MAP_SIZE 2048
#define FIELD_SIZE 512

// Room round the scene, and below its lowest point, world units
#define MARGIN 40.0f
#define BELOW 100.0f
#define ABOVE 60.0f

static GLuint g_texture;
static GLuint g_fbo;
static int g_ready;
// Where the map sits: its corner and size on the ground, and its height span
static float g_origin[2];
static float g_size[2];
static float g_top;
static float g_range;

static const char* GLSL =
	"uniform sampler2D u_cover;\n"
	"uniform vec2 u_cover_origin;\n"
	"uniform vec2 u_cover_scale;\n"
	"uniform vec2 u_cover_top;\n"
	"\n"
	"float cover_height(vec2 xz) {\n"
	"    vec2 uv = (xz - u_cover_origin) * u_cover_scale;\n"
	"    if (u_cover_top.y <= 0.0 || uv.x < 0.0 || uv.y < 0.0 || uv.x > 1.0 || uv.y > 1.0)\n"
	"        return -1000.0;\n"
	"    return u_cover_top.x - textureLod(u_cover, uv, 0.0).r * u_cover_top.y;\n"
	"}\n"
	"\n";

const char* cover_map_glsl(void) {
	return GLSL;
}

// The matrix that draws the scene from above into the map: x across, ground
// y up the map, and height as depth with the top of the scene nearest
static void top_down_matrix(float m[16]) {
	for(int i = 0; i < 16; i++)
		m[i] = 0.0f;
	m[0] = 2.0f / g_size[0];
	m[12] = -(2.0f * g_origin[0] + g_size[0]) / g_size[0];
	m[9] = 2.0f / g_size[1];
	m[13] = -(2.0f * g_origin[1] + g_size[1]) / g_size[1];
	m[6] = -2.0f / g_range;
	m[14] = 2.0f * g_top / g_range - 1.0f;
	m[15] = 1.0f;
}

static int make_target(void) {
	GLenum status;
	GLint previous = 0;

	glGenTextures(1, &g_texture);
	glBindTexture(GL_TEXTURE_2D, g_texture);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, MAP_SIZE, MAP_SIZE, 0,
	             GL_DEPTH_COMPONENT, GL_FLOAT, NULL);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_NONE);
	glBindTexture(GL_TEXTURE_2D, 0);

	glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previous);
	glGenFramebuffers(1, &g_fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, g_texture, 0);
	glDrawBuffer(GL_NONE);
	glReadBuffer(GL_NONE);
	status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)previous);

	return status == GL_FRAMEBUFFER_COMPLETE;
}

// Keep a coarse copy for the code with no GPU: the highest surface in each block
static void keep_coarse_copy(void) {
	const int block = MAP_SIZE / FIELD_SIZE;
	float* depth = malloc((size_t)MAP_SIZE * MAP_SIZE * sizeof(float));
	float* heights = malloc((size_t)FIELD_SIZE * FIELD_SIZE * sizeof(float));

	if(depth && heights) {
		glBindTexture(GL_TEXTURE_2D, g_texture);
		glGetTexImage(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT, GL_FLOAT, depth);
		glBindTexture(GL_TEXTURE_2D, 0);
		for(int cy = 0; cy < FIELD_SIZE; cy++) {
			for(int cx = 0; cx < FIELD_SIZE; cx++) {
				float nearest = 1.0f;

				for(int y = 0; y < block; y++)
					for(int x = 0; x < block; x++) {
						float d = depth[(cy * block + y) * MAP_SIZE + cx * block + x];

						if(d < nearest)
							nearest = d;
					}
				heights[cy * FIELD_SIZE + cx] = g_top - nearest * g_range;
			}
		}
		cover_field_set(heights, FIELD_SIZE, FIELD_SIZE, g_origin[0], g_origin[1],
		                g_size[0] / FIELD_SIZE, g_size[1] / FIELD_SIZE);
	}
	free(depth);
	free(heights);
}

int cover_map_build(void) {
	float min_x, min_y, max_x, max_y, low, high;
	float mvp[16];

	cover_map_shutdown();
	if(!shadow_map_ready() || !city_renderer_bounds(&min_x, &min_y, &max_x, &max_y) ||
	   !city_renderer_height_range(&low, &high))
		return 0;
	g_origin[0] = min_x - MARGIN;
	g_origin[1] = min_y - MARGIN;
	g_size[0] = max_x - min_x + 2.0f * MARGIN;
	g_size[1] = max_y - min_y + 2.0f * MARGIN;
	g_top = high + ABOVE;
	g_range = g_top - (low - BELOW);
	if(!make_target()) {
		CLOG_ERROR("[COVER] The cover map framebuffer is incomplete; rain is not stopped by roofs");
		cover_map_shutdown();
		return 0;
	}

	top_down_matrix(mvp);
	shadow_map_begin_target(g_fbo, 0, 0, MAP_SIZE, MAP_SIZE);
	city_renderer_draw_shadow(mvp);
	shadow_map_end();
	keep_coarse_copy();
	g_ready = 1;
	CLOG_INFO("[COVER] Map ready: %dx%d texels over %.0f x %.0f units", MAP_SIZE, MAP_SIZE,
	          g_size[0], g_size[1]);

	return 1;
}

void cover_map_shutdown(void) {
	if(g_fbo)
		glDeleteFramebuffers(1, &g_fbo);
	if(g_texture)
		glDeleteTextures(1, &g_texture);
	g_fbo = 0;
	g_texture = 0;
	g_ready = 0;
	cover_field_clear();
}

int cover_map_ready(void) {
	return g_ready;
}

void cover_map_locate(GLuint program, cover_uniforms* out) {
	out->texture = glGetUniformLocation(program, "u_cover");
	out->origin = glGetUniformLocation(program, "u_cover_origin");
	out->scale = glGetUniformLocation(program, "u_cover_scale");
	out->top = glGetUniformLocation(program, "u_cover_top");
}

void cover_map_apply(const cover_uniforms* u) {
	glActiveTexture(GL_TEXTURE0 + COVER_MAP_UNIT);
	glBindTexture(GL_TEXTURE_2D, g_ready ? g_texture : 0);
	glActiveTexture(GL_TEXTURE0);
	glUniform1i(u->texture, COVER_MAP_UNIT);
	glUniform2f(u->origin, g_origin[0], g_origin[1]);
	glUniform2f(u->scale, g_ready ? 1.0f / g_size[0] : 0.0f, g_ready ? 1.0f / g_size[1] : 0.0f);
	// A range of 0 tells the shader there is no map
	glUniform2f(u->top, g_top, g_ready ? g_range : 0.0f);
}
