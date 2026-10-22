/**
 * @file
 * Render the city from above into a texture for the map.
 */
#include "ui/map_terrain.h"
#include "render/gl_loader.h"
#include "render/city_renderer.h"
#include "camera/cglm_config.h"
#include "core/client_log.h"

#include <math.h>
#include <string.h>

// The largest image the terrain is drawn at, on either side.
#define MAX_TERRAIN_PX 2048

// Eye height above the scene and the depth range that holds all of it.
#define EYE_HEIGHT 8000.0f
#define NEAR_PLANE 1.0f
#define FAR_PLANE  20000.0f

// A view this close to the last one is the last one, in world units.
#define SAME_EPSILON 0.05f

void map_terrain_release(map_terrain_target* t) {
	GLuint fbo = t->fbo;
	GLuint depth = t->depth;
	GLuint color = t->color;

	if(fbo)
		glDeleteFramebuffers(1, &fbo);
	if(depth)
		glDeleteRenderbuffers(1, &depth);
	if(color)
		glDeleteTextures(1, &color);
	memset(t, 0, sizeof(*t));
}

static void make_color_texture(map_terrain_target* t, int width, int height) {
	GLuint color = 0;

	glGenTextures(1, &color);
	t->color = color;
	glBindTexture(GL_TEXTURE_2D, color);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA,
	             GL_UNSIGNED_BYTE, NULL);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glBindTexture(GL_TEXTURE_2D, 0);
}

// Build the framebuffer for an image size. Returns 1 when it is complete.
static int make_targets(map_terrain_target* t, int width, int height) {
	GLuint depth = 0;
	GLuint fbo = 0;

	map_terrain_release(t);
	make_color_texture(t, width, height);

	glGenRenderbuffers(1, &depth);
	t->depth = depth;
	glBindRenderbuffer(GL_RENDERBUFFER, depth);
	glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, width, height);
	glBindRenderbuffer(GL_RENDERBUFFER, 0);

	GLint previous = 0;

	glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previous);
	glGenFramebuffers(1, &fbo);
	t->fbo = fbo;
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
	                       t->color, 0);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depth);

	int complete = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;

	glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)previous);
	if(!complete) {
		CLOG_ERROR("[MAP] Framebuffer %dx%d is incomplete", width, height);
		map_terrain_release(t);
		return 0;
	}
	t->width = width;
	t->height = height;

	return 1;
}

static int same_view(const map_terrain_view* a, const map_terrain_view* b) {
	return fabsf(a->center_x - b->center_x) < SAME_EPSILON &&
	       fabsf(a->center_y - b->center_y) < SAME_EPSILON &&
	       fabsf(a->half_w - b->half_w) < SAME_EPSILON &&
	       fabsf(a->half_h - b->half_h) < SAME_EPSILON &&
	       fabsf(a->clip_y - b->clip_y) < SAME_EPSILON &&
	       a->width == b->width && a->height == b->height;
}

// The clip-from-world matrix looking straight down, north (-y) up the image.
static void build_view_projection(const map_terrain_view* view, mat4 out) {
	mat4 proj, look;
	vec3 eye = { view->center_x, EYE_HEIGHT, view->center_y };
	vec3 at = { view->center_x, 0.0f, view->center_y };
	vec3 up = { 0.0f, 0.0f, -1.0f };

	glm_ortho(-view->half_w, view->half_w, -view->half_h, view->half_h,
	          NEAR_PLANE, FAR_PLANE, proj);
	glm_lookat(eye, at, up, look);
	glm_mat4_mul(proj, look, out);
}

// Draw the city into the bound framebuffer, then put the caller's state back.
static void draw_into_targets(const map_terrain_target* t, const map_terrain_view* view) {
	GLint previous = 0;
	GLint viewport[4];
	GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
	mat4 viewproj;

	glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previous);
	glGetIntegerv(GL_VIEWPORT, viewport);
	glDisable(GL_SCISSOR_TEST);

	glBindFramebuffer(GL_FRAMEBUFFER, t->fbo);
	glViewport(0, 0, t->width, t->height);
	glClearColor(0.035f, 0.045f, 0.065f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

	build_view_projection(view, viewproj);
	city_renderer_draw_map((const float*)viewproj, view->clip_y);

	glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)previous);
	glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
	if(scissor)
		glEnable(GL_SCISSOR_TEST);
}

unsigned int map_terrain_render(map_terrain_target* t, const map_terrain_view* view) {
	if(!view || view->width < 16 || view->height < 16)
		return 0;
	if(t->width != view->width || t->height != view->height) {
		if(!make_targets(t, view->width, view->height))
			return 0;
	}
	if(t->have_last && same_view(&t->last, view))
		return t->color;

	draw_into_targets(t, view);
	t->last = *view;
	t->have_last = 1;

	return t->color;
}

unsigned int map_terrain_render_area(map_terrain_target* t, const map_projection* proj,
                                     float viewport_w, float clip_y) {
	GLint viewport[4];
	map_terrain_view view;
	const map_rect* a = &proj->area;

	glGetIntegerv(GL_VIEWPORT, viewport);

	float scale = (float)viewport[2] / viewport_w;

	view.center_x = proj->cam_x;
	view.center_y = proj->cam_y;
	view.half_w = a->w * 0.5f / proj->k;
	view.half_h = a->h * 0.5f / proj->k;
	view.clip_y = clip_y;
	view.width = (int)fminf(fmaxf(a->w * scale + 0.5f, 16.0f), MAX_TERRAIN_PX);
	view.height = (int)fminf(fmaxf(a->h * scale + 0.5f, 16.0f), MAX_TERRAIN_PX);

	return map_terrain_render(t, &view);
}
