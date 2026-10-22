// The depth maps the shadows are drawn into.

#include "render/gl_loader.h"
#include "render/shadow_map.h"
#include "core/client_log.h"

static const char* VERTEX_SRC =
	"#version 330 core\n"
	"layout(location = 0) in vec3 a_pos;\n"
	"uniform mat4 u_mvp;\n"
	"void main() {\n"
	"    gl_Position = u_mvp * vec4(a_pos, 1.0);\n"
	"}\n";

static const char* FRAGMENT_SRC =
	"#version 330 core\n"
	"void main() {\n"
	"}\n";

static GLuint g_program;
static GLint u_mvp;
static GLuint g_texture[SHADOW_CASCADES];
static GLuint g_fbo[SHADOW_CASCADES];
static int g_ready;

// What was bound before shadow_map_begin, to restore
static GLint g_prev_fbo;
static GLint g_prev_viewport[4];
static GLint g_prev_scissor_box[4];
static GLboolean g_prev_scissor;

static GLuint compile(GLenum type, const char* src) {
	GLuint shader = glCreateShader(type);
	GLint ok = 0;

	glShaderSource(shader, 1, &src, NULL);
	glCompileShader(shader);
	glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
	if(!ok) {
		char log[1024];

		glGetShaderInfoLog(shader, (GLsizei)sizeof(log), NULL, log);
		CLOG_ERROR("[SHADOW] %s shader failed to compile: %s",
		           type == GL_VERTEX_SHADER ? "vertex" : "fragment", log);
		glDeleteShader(shader);
		return 0;
	}

	return shader;
}

static int build_program(void) {
	GLuint vs = compile(GL_VERTEX_SHADER, VERTEX_SRC);
	GLuint fs = compile(GL_FRAGMENT_SHADER, FRAGMENT_SRC);
	GLint ok = 0;

	if(!vs || !fs) {
		if(vs)
			glDeleteShader(vs);
		if(fs)
			glDeleteShader(fs);
		return 0;
	}
	g_program = glCreateProgram();
	glAttachShader(g_program, vs);
	glAttachShader(g_program, fs);
	glLinkProgram(g_program);
	glDeleteShader(vs);
	glDeleteShader(fs);
	glGetProgramiv(g_program, GL_LINK_STATUS, &ok);
	if(!ok) {
		char log[1024];

		glGetProgramInfoLog(g_program, (GLsizei)sizeof(log), NULL, log);
		CLOG_ERROR("[SHADOW] Shader program failed to link: %s", log);
		glDeleteProgram(g_program);
		g_program = 0;
		return 0;
	}
	u_mvp = glGetUniformLocation(g_program, "u_mvp");

	return 1;
}

// One depth texture and the framebuffer that holds only it. Nonzero when complete.
static int make_map(int cascade) {
	GLenum status;

	glGenTextures(1, &g_texture[cascade]);
	glBindTexture(GL_TEXTURE_2D, g_texture[cascade]);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, SHADOW_MAP_SIZE, SHADOW_MAP_SIZE, 0,
	             GL_DEPTH_COMPONENT, GL_FLOAT, NULL);
	// Linear filtering on a compare texture is the hardware's 2x2 soft edge
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
	glBindTexture(GL_TEXTURE_2D, 0);

	glGenFramebuffers(1, &g_fbo[cascade]);
	glBindFramebuffer(GL_FRAMEBUFFER, g_fbo[cascade]);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D,
	                       g_texture[cascade], 0);
	// Depth only: no colour to draw or read
	glDrawBuffer(GL_NONE);
	glReadBuffer(GL_NONE);
	status = glCheckFramebufferStatus(GL_FRAMEBUFFER);

	return status == GL_FRAMEBUFFER_COMPLETE;
}

int shadow_map_init(void) {
	GLint previous = 0;
	int ok = 1;

	if(!build_program())
		return 0;
	glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previous);
	for(int c = 0; c < SHADOW_CASCADES; c++)
		if(!make_map(c))
			ok = 0;
	glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)previous);
	if(!ok) {
		CLOG_ERROR("[SHADOW] The shadow framebuffer is incomplete; shadows are off");
		shadow_map_shutdown();
		return 0;
	}
	g_ready = 1;
	CLOG_INFO("[SHADOW] %d maps of %dx%d ready", SHADOW_CASCADES, SHADOW_MAP_SIZE,
	          SHADOW_MAP_SIZE);

	return 1;
}

void shadow_map_shutdown(void) {
	for(int c = 0; c < SHADOW_CASCADES; c++) {
		if(g_fbo[c])
			glDeleteFramebuffers(1, &g_fbo[c]);
		if(g_texture[c])
			glDeleteTextures(1, &g_texture[c]);
		g_fbo[c] = 0;
		g_texture[c] = 0;
	}
	if(g_program) {
		glDeleteProgram(g_program);
		g_program = 0;
	}
	g_ready = 0;
}

int shadow_map_ready(void) {
	return g_ready;
}

void shadow_map_begin_target(unsigned int fbo, int x, int y, int w, int h) {
	glGetIntegerv(GL_FRAMEBUFFER_BINDING, &g_prev_fbo);
	glGetIntegerv(GL_VIEWPORT, g_prev_viewport);
	glGetIntegerv(GL_SCISSOR_BOX, g_prev_scissor_box);
	g_prev_scissor = glIsEnabled(GL_SCISSOR_TEST);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glViewport(x, y, w, h);
	// Only this rectangle is cleared, so the others on the same texture are kept
	glEnable(GL_SCISSOR_TEST);
	glScissor(x, y, w, h);
	glEnable(GL_DEPTH_TEST);
	glDepthFunc(GL_LESS);
	glDepthMask(GL_TRUE);
	glClear(GL_DEPTH_BUFFER_BIT);
	// Thin walls and hair are seen from both sides, so nothing is culled
	glDisable(GL_CULL_FACE);
	glEnable(GL_POLYGON_OFFSET_FILL);
	glPolygonOffset(2.0f, 4.0f);
	glUseProgram(g_program);
}

void shadow_map_begin(int cascade) {
	shadow_map_begin_target(g_fbo[cascade], 0, 0, SHADOW_MAP_SIZE, SHADOW_MAP_SIZE);
}

void shadow_map_set_mvp(const float mvp[16]) {
	glUniformMatrix4fv(u_mvp, 1, GL_FALSE, mvp);
}

void shadow_map_end(void) {
	glUseProgram(0);
	glDisable(GL_POLYGON_OFFSET_FILL);
	glDisable(GL_DEPTH_TEST);
	glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)g_prev_fbo);
	glViewport(g_prev_viewport[0], g_prev_viewport[1], g_prev_viewport[2], g_prev_viewport[3]);
	glScissor(g_prev_scissor_box[0], g_prev_scissor_box[1], g_prev_scissor_box[2],
	          g_prev_scissor_box[3]);
	if(!g_prev_scissor)
		glDisable(GL_SCISSOR_TEST);
}

unsigned int shadow_map_texture(int cascade) {
	return g_texture[cascade];
}
