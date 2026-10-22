// Draws the player's body into small textures for the emote hub.

#include "ui/emote_preview.h"
#include "player/emote_clips.h"
#include "render/character_renderer.h"
#include "render/scene_light.h"
#include "render/character_tuning.h"
#include "render/gl_loader.h"
#include "camera/cglm_config.h"
#include "core/client_log.h"

#include <math.h>
#include <string.h>

// The isometric view: where it looks, from which side, and how much it frames
#define VIEW_TARGET_Y 15.0f
#define VIEW_YAW 0.70f
#define VIEW_PITCH 0.60f
#define VIEW_DISTANCE 300.0f
#define VIEW_HALF_HEIGHT 22.0f

static GLuint s_fbo;
static GLuint s_depth;
static GLuint s_color[EMOTE_PREVIEW_CELLS];
static int s_warned; // the incomplete-framebuffer warning was written

void emote_preview_release(void) {
	GLuint fbo = s_fbo;
	GLuint depth = s_depth;

	if(fbo)
		glDeleteFramebuffers(1, &fbo);
	if(depth)
		glDeleteRenderbuffers(1, &depth);
	for(int i = 0; i < EMOTE_PREVIEW_CELLS; i++) {
		GLuint color = s_color[i];

		if(color)
			glDeleteTextures(1, &color);
	}

	s_fbo = 0;
	s_depth = 0;
	memset(s_color, 0, sizeof(s_color));
}

unsigned int emote_preview_texture(int cell) {
	if(cell < 0 || cell >= EMOTE_PREVIEW_CELLS)
		return 0;

	return s_color[cell];
}

// The texture for a cell, made when first wanted
static GLuint cell_texture(int cell) {
	GLuint color = 0;

	if(s_color[cell])
		return s_color[cell];

	glGenTextures(1, &color);
	glBindTexture(GL_TEXTURE_2D, color);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, EMOTE_PREVIEW_PX_W, EMOTE_PREVIEW_PX_H, 0, GL_RGBA,
	             GL_UNSIGNED_BYTE, NULL);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glBindTexture(GL_TEXTURE_2D, 0);
	s_color[cell] = color;

	return color;
}

// The framebuffer and the depth every cell is drawn with; 1 when ready
static int ensure_target(void) {
	GLuint depth = 0;
	GLuint fbo = 0;

	if(s_fbo)
		return 1;

	glGenRenderbuffers(1, &depth);
	glBindRenderbuffer(GL_RENDERBUFFER, depth);
	glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, EMOTE_PREVIEW_PX_W,
	                      EMOTE_PREVIEW_PX_H);
	glBindRenderbuffer(GL_RENDERBUFFER, 0);
	glGenFramebuffers(1, &fbo);
	s_depth = depth;
	s_fbo = fbo;

	return 1;
}

// The view every cell is drawn from; only the matrix is read
static void build_view(CameraView* view) {
	float aspect = (float)EMOTE_PREVIEW_PX_W / (float)EMOTE_PREVIEW_PX_H;
	vec3 target = { 0.0f, VIEW_TARGET_Y, 0.0f };
	vec3 eye = {
		VIEW_DISTANCE * cosf(VIEW_PITCH) * sinf(VIEW_YAW),
		VIEW_TARGET_Y + VIEW_DISTANCE * sinf(VIEW_PITCH),
		VIEW_DISTANCE * cosf(VIEW_PITCH) * cosf(VIEW_YAW),
	};
	vec3 up = { 0.0f, 1.0f, 0.0f };
	mat4 proj;
	mat4 look;
	mat4 both;

	glm_ortho(-VIEW_HALF_HEIGHT * aspect, VIEW_HALF_HEIGHT * aspect, -VIEW_HALF_HEIGHT,
	          VIEW_HALF_HEIGHT, 1.0f, VIEW_DISTANCE * 2.0f, proj);
	glm_lookat(eye, target, up, look);
	glm_mat4_mul(proj, look, both);
	memset(view, 0, sizeof(*view));
	memcpy(view->viewproj, both, sizeof(view->viewproj));
	view->viewport_w = EMOTE_PREVIEW_PX_W;
	view->viewport_h = EMOTE_PREVIEW_PX_H;
}

// What the body does in a box; gaits run at the speed their clips are made for
static CharacterStyle style_for(const preview_job* job, const char* race_key,
		uint32_t appearance) {
	CharacterStyle style;
	unsigned char pick = (unsigned char)(job->clip + 1);

	memset(&style, 0, sizeof(style));
	style.scale = 1.0f;
	style.race_key = race_key;
	style.appearance = appearance;
	// no foot IK: there is no floor under a box
	style.hints.airborne = 1;
	style.hints.has_facing = 1;

	switch(job->kind) {
	case PREVIEW_WALK:
		style.hints.has_speed = 1;
		style.hints.speed = CHARACTER_WALK_CLIP_SPEED;
		style.hints.clip_walk = pick;
		break;
	case PREVIEW_RUN:
		style.hints.has_speed = 1;
		style.hints.speed = CHARACTER_JOG_CLIP_SPEED;
		style.hints.clip_jog = pick;
		break;
	case PREVIEW_SIT:
		style.hints.has_pose = 1;
		style.hints.pose = CHARACTER_ANIM_SIT_IDLE;
		style.hints.clip_sit = pick;
		break;
	default:
		style.hints.has_pose = 1;
		if(job->clip == EMOTE_CLIP_LEAN_1 || job->clip == EMOTE_CLIP_LEAN_2) {
			// a whole emote shows the pose it is held in
			style.hints.pose = job->clip == EMOTE_CLIP_LEAN_1 ? CHARACTER_ANIM_LEAN_IDLE
			                                                 : CHARACTER_ANIM_LEAN_FOOT_IDLE;
			break;
		}
		style.hints.pose = CHARACTER_ANIM_CUSTOM;
		style.hints.clip_custom = pick;
		break;
	}

	return style;
}

static void draw_cell(const preview_job* job, const char* race_key, uint32_t appearance) {
	CharacterStyle style = style_for(job, race_key, appearance);

	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
	                       cell_texture(job->cell), 0);
	if(glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
		if(!s_warned)
			CLOG_WARN("[EMOTES] The preview framebuffer is incomplete");
		s_warned = 1;
		return;
	}

	glClearColor(0.10f, 0.11f, 0.16f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	character_renderer_draw(CHARACTER_KIND_PREVIEW, job->body, 0.0f, 0.0f, 0.0f, &style);
}

void emote_preview_render(const preview_job* jobs, int count, const char* race_key,
		uint32_t appearance, float dt) {
	GLint previous = 0;
	GLint viewport[4];
	GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
	CameraView view;

	if(count <= 0 || !character_renderer_ready() || !ensure_target())
		return;

	glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previous);
	glGetIntegerv(GL_VIEWPORT, viewport);
	glDisable(GL_SCISSOR_TEST);
	glBindFramebuffer(GL_FRAMEBUFFER, s_fbo);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, s_depth);
	glViewport(0, 0, EMOTE_PREVIEW_PX_W, EMOTE_PREVIEW_PX_H);

	build_view(&view);
	// The previews show a body in plain daylight, not the world's hour or shadows
	scene_light_set_flat(1);
	character_renderer_begin(&view, 0.0f, 0.0f, dt);
	for(int i = 0; i < count; i++)
		if(jobs[i].cell >= 0 && jobs[i].cell < EMOTE_PREVIEW_CELLS)
			draw_cell(&jobs[i], race_key, appearance);
	character_renderer_end();
	scene_light_set_flat(0);

	glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)previous);
	glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
	if(scissor)
		glEnable(GL_SCISSOR_TEST);
}
