// Draw the lamps' shadows into one atlas.

#include "render/gl_loader.h"
#include "render/lamp_shadow.h"
#include "render/shadow_map.h"
#include "render/city_renderer.h"
#include "render/character_renderer.h"
#include "world/lamp_shadow_math.h"
#include "core/client_log.h"

#include <math.h>

#define ATLAS_W (LAMP_SHADOW_TILE * LAMP_SHADOW_FACES)
#define ATLAS_H (LAMP_SHADOW_TILE * LAMP_SHADOW_MAX_LAMPS)

#ifndef GL_MAX_TEXTURE_SIZE
#define GL_MAX_TEXTURE_SIZE 0x0D33
#endif

static GLuint g_texture;
static GLuint g_fbo;
static int g_ready;
static int g_active;
static city_lamp g_lamps[LAMP_SHADOW_MAX_LAMPS];
static int g_count;

int lamp_shadow_init(void) {
	GLint previous = 0;
	GLint max_size = 0;
	GLenum status;

	if(!shadow_map_ready())
		return 0;
	glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_size);
	if(max_size < ATLAS_H) {
		CLOG_WARN("[LAMPS] Textures of %d are too small for lamp shadows (need %d)", max_size,
		          ATLAS_H);
		return 0;
	}
	glGenTextures(1, &g_texture);
	glBindTexture(GL_TEXTURE_2D, g_texture);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, ATLAS_W, ATLAS_H, 0, GL_DEPTH_COMPONENT,
	             GL_FLOAT, NULL);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
	glBindTexture(GL_TEXTURE_2D, 0);

	glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previous);
	glGenFramebuffers(1, &g_fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, g_texture, 0);
	glDrawBuffer(GL_NONE);
	glReadBuffer(GL_NONE);
	status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)previous);
	if(status != GL_FRAMEBUFFER_COMPLETE) {
		CLOG_ERROR("[LAMPS] The lamp shadow framebuffer is incomplete; lamps cast no shadows");
		lamp_shadow_shutdown();
		return 0;
	}
	g_ready = 1;
	CLOG_INFO("[LAMPS] Shadows for up to %d lamps ready (%dx%d)", LAMP_SHADOW_MAX_LAMPS, ATLAS_W,
	          ATLAS_H);

	return 1;
}

void lamp_shadow_shutdown(void) {
	if(g_fbo)
		glDeleteFramebuffers(1, &g_fbo);
	if(g_texture)
		glDeleteTextures(1, &g_texture);
	g_fbo = 0;
	g_texture = 0;
	g_ready = 0;
	g_active = 0;
}

// Draw one lamp's six views into its row of the atlas
static void draw_lamp(int slot, const city_lamp* lamp) {
	float viewproj[LAMP_SHADOW_FACES][16];

	lamp_shadow_fit(lamp->pos, LAMP_SHADOW_NEAR, lamp->radius, viewproj);
	for(int face = 0; face < LAMP_SHADOW_FACES; face++) {
		shadow_map_begin_target(g_fbo, face * LAMP_SHADOW_TILE, slot * LAMP_SHADOW_TILE,
		                        LAMP_SHADOW_TILE, LAMP_SHADOW_TILE);
		city_renderer_draw_shadow(viewproj[face]);
		character_renderer_draw_shadows_near(viewproj[face], lamp->pos[0], lamp->pos[2],
		                                     lamp->radius);
		shadow_map_end();
	}
}

void lamp_shadow_run(float lamp_amount, float focus_x, float focus_y) {
	city_lamp nearest[LAMP_SHADOW_MAX_LAMPS];
	int found;

	g_count = 0;
	g_active = 0;
	if(lamp_amount < 0.01f)
		return;
	found = city_lamps_nearest(focus_x, focus_y, LAMP_SHADOW_MAX_LAMPS, nearest);
	for(int i = 0; i < found; i++) {
		float dx = nearest[i].pos[0] - focus_x;
		float dy = nearest[i].pos[2] - focus_y;

		// Nearest first, so the first one out of range ends the list
		if(dx * dx + dy * dy > LAMP_SHADOW_RANGE * LAMP_SHADOW_RANGE)
			break;
		g_lamps[g_count++] = nearest[i];
	}
	if(!g_ready)
		return;
	for(int i = 0; i < g_count; i++)
		draw_lamp(i, &g_lamps[i]);
	g_active = g_count > 0;
}

int lamp_shadow_lamps(const city_lamp** out) {
	*out = g_lamps;

	return g_count;
}

int lamp_shadow_active(void) {
	return g_active;
}

unsigned int lamp_shadow_texture(void) {
	return g_texture;
}
