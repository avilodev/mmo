// Draws the selected character's body into a texture for character select.

#include "ui/char_select_preview.h"
#include "render/character_renderer.h"
#include "render/scene_light.h"
#include "render/character_tuning.h"
#include "render/gl_loader.h"
#include "camera/camera_math.h"
#include "core/client_log.h"

#include <string.h>

// The body's id among the preview bodies; the emote hub uses small ones
#define PREVIEW_BODY_ID 5000u

// The camera: level with the chest, a little way off, looking straight on
#define VIEW_TARGET_H 21.0f
#define VIEW_PITCH 0.04f
#define VIEW_DISTANCE 110.0f
#define VIEW_FOV 0.50f

static GLuint s_fbo;
static GLuint s_depth;
static GLuint s_color;
static int s_warned; // the incomplete-framebuffer warning was written

void char_select_preview_release(void) {
	GLuint fbo = s_fbo;
	GLuint depth = s_depth;
	GLuint color = s_color;

	if(fbo)
		glDeleteFramebuffers(1, &fbo);

	if(depth)
		glDeleteRenderbuffers(1, &depth);

	if(color)
		glDeleteTextures(1, &color);

	s_fbo = 0;
	s_depth = 0;
	s_color = 0;
}

unsigned int char_select_preview_texture(void) {
	return s_color;
}

// The texture, depth and framebuffer, made when first wanted; 1 when ready
static int ensure_target(void) {
	GLuint fbo = 0;
	GLuint depth = 0;
	GLuint color = 0;

	if(s_fbo)
		return 1;

	glGenTextures(1, &color);
	glBindTexture(GL_TEXTURE_2D, color);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, CS_PREVIEW_PX_W, CS_PREVIEW_PX_H, 0, GL_RGBA,
	             GL_UNSIGNED_BYTE, NULL);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glBindTexture(GL_TEXTURE_2D, 0);

	glGenRenderbuffers(1, &depth);
	glBindRenderbuffer(GL_RENDERBUFFER, depth);
	glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, CS_PREVIEW_PX_W,
	                      CS_PREVIEW_PX_H);
	glBindRenderbuffer(GL_RENDERBUFFER, 0);

	glGenFramebuffers(1, &fbo);
	s_color = color;
	s_depth = depth;
	s_fbo = fbo;

	return 1;
}

static CharacterStyle make_style(const char* race_key, uint32_t appearance, float yaw) {
	CharacterStyle style;

	memset(&style, 0, sizeof(style));
	style.scale = 1.0f;
	style.race_key = race_key;
	style.appearance = appearance;

	// no foot IK: there is no floor under the picture
	style.hints.airborne = 1;
	style.hints.has_facing = 1;
	style.hints.facing_yaw = yaw;

	return style;
}

void char_select_preview_render(const char* race_key, uint32_t appearance, float yaw,
		float dt) {
	GLint previous = 0;
	GLint viewport[4];
	GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
	CameraView view;
	CharacterStyle style = make_style(race_key, appearance, yaw);

	if(!character_renderer_ready() || !ensure_target())
		return;

	glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previous);
	glGetIntegerv(GL_VIEWPORT, viewport);
	glDisable(GL_SCISSOR_TEST);
	glBindFramebuffer(GL_FRAMEBUFFER, s_fbo);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, s_depth);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_color, 0);

	if(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
		glViewport(0, 0, CS_PREVIEW_PX_W, CS_PREVIEW_PX_H);
		camera_view_build(&view, 0.0f, 0.0f, VIEW_TARGET_H, 0.0f, VIEW_PITCH, VIEW_DISTANCE,
		                  VIEW_FOV, CS_PREVIEW_PX_W, CS_PREVIEW_PX_H);

		// Clear to nothing, so the screen's own backdrop shows around the body
		glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

		// The body in plain daylight, not the world's hour or shadows
		scene_light_set_flat(1);
		character_renderer_begin(&view, 0.0f, 0.0f, dt);
		character_renderer_draw(CHARACTER_KIND_PREVIEW, PREVIEW_BODY_ID, 0.0f, 0.0f, 0.0f,
		                        &style);
		character_renderer_end();
		scene_light_set_flat(0);
	} else if(!s_warned) {
		CLOG_WARN("[CHAR_SELECT] The preview framebuffer is incomplete");
		s_warned = 1;
	}

	glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)previous);
	glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);

	if(scissor)
		glEnable(GL_SCISSOR_TEST);
}
