// The patches of weather as a texture.

#include "render/gl_loader.h"
#include "render/weather_map.h"
#include "core/client_log.h"
#include "world/weather_cells.h"
#include "world/weather_now.h"

#include <math.h>
#include <string.h>

// Texels along a side: 96 over 12000 units is a texel every 125 units
#define MAP_SIZE 96

// How often the map is rebuilt, in seconds of the shared clock. The patches
// drift 48 units a second, so a rebuild a fifth of a second apart moves them
// by less than a tenth of a texel.
#define REBUILD_EVERY_S 0.2

static GLuint g_texture;
static int g_ready;
static float g_origin[2];
static double g_built_at;
static int g_built;
static float g_peak_rain;
static float g_peak_snow;
static weather_cell_sample g_samples[MAP_SIZE * MAP_SIZE];
static unsigned char g_pixels[MAP_SIZE * MAP_SIZE * 4];

static const char* GLSL =
	"uniform sampler2D u_wmap;\n"
	"uniform vec2 u_wmap_origin;\n"
	"uniform float u_wmap_scale;\n"
	"uniform vec4 u_wmap_global;\n"
	"\n"
	"vec4 weather_at(vec2 xz) {\n"
	"    if (u_wmap_scale <= 0.0) return u_wmap_global;\n"
	"    vec2 uv = clamp((xz - u_wmap_origin) * u_wmap_scale, 0.0, 1.0);\n"
	"    return textureLod(u_wmap, uv, 0.0);\n"
	"}\n"
	"\n";

const char* weather_map_glsl(void) {
	return GLSL;
}

int weather_map_init(void) {
	glGenTextures(1, &g_texture);
	glBindTexture(GL_TEXTURE_2D, g_texture);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, MAP_SIZE, MAP_SIZE, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glBindTexture(GL_TEXTURE_2D, 0);
	g_ready = 0;
	g_built = 0;
	CLOG_INFO("[WMAP] Weather map %dx%d over %.0f units", MAP_SIZE, MAP_SIZE, WEATHER_MAP_SPAN);

	return 1;
}

void weather_map_shutdown(void) {
	if(g_texture)
		glDeleteTextures(1, &g_texture);
	g_texture = 0;
	g_ready = 0;
	g_built = 0;
}

static unsigned char to_byte(float v) {
	if(v < 0.0f)
		v = 0.0f;
	if(v > 1.0f)
		v = 1.0f;

	return (unsigned char)(v * 255.0f + 0.5f);
}

void weather_map_update(float focus_x, float focus_y) {
	const weather_cells_input* in = weather_now_cells();
	float cell = WEATHER_MAP_SPAN / (float)MAP_SIZE;
	float origin_x = floorf((focus_x - WEATHER_MAP_SPAN * 0.5f) / cell) * cell;
	float origin_y = floorf((focus_y - WEATHER_MAP_SPAN * 0.5f) / cell) * cell;

	if(!g_texture)
		return;
	// Rebuilt when the clock has moved on, or the focus has crossed into another texel
	if(g_built && fabs(in->shared_s - g_built_at) < REBUILD_EVERY_S && origin_x == g_origin[0] &&
	   origin_y == g_origin[1])
		return;

	// A texel's value is the weather at its middle
	weather_cells_grid(in, origin_x + cell * 0.5f, origin_y + cell * 0.5f, cell, MAP_SIZE, g_samples);
	g_peak_rain = 0.0f;
	g_peak_snow = 0.0f;
	for(int i = 0; i < MAP_SIZE * MAP_SIZE; i++) {
		const weather_cell_sample* s = &g_samples[i];

		g_pixels[i * 4] = to_byte(s->cloud);
		g_pixels[i * 4 + 1] = to_byte(s->rain);
		g_pixels[i * 4 + 2] = to_byte(s->snow);
		g_pixels[i * 4 + 3] = to_byte(s->storm);
		if(s->rain > g_peak_rain)
			g_peak_rain = s->rain;
		if(s->snow > g_peak_snow)
			g_peak_snow = s->snow;
	}
	glBindTexture(GL_TEXTURE_2D, g_texture);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, MAP_SIZE, MAP_SIZE, GL_RGBA, GL_UNSIGNED_BYTE, g_pixels);
	glBindTexture(GL_TEXTURE_2D, 0);
	g_origin[0] = origin_x;
	g_origin[1] = origin_y;
	g_built_at = in->shared_s;
	g_built = 1;
	g_ready = 1;
}

float weather_map_peak_rain(void) {
	return g_ready ? g_peak_rain : weather_now_get()->rain;
}

float weather_map_peak_snow(void) {
	return g_ready ? g_peak_snow : weather_now_get()->snow;
}

void weather_map_locate(GLuint program, weather_map_uniforms* out) {
	out->texture = glGetUniformLocation(program, "u_wmap");
	out->origin = glGetUniformLocation(program, "u_wmap_origin");
	out->scale = glGetUniformLocation(program, "u_wmap_scale");
	out->global = glGetUniformLocation(program, "u_wmap_global");
}

void weather_map_apply(const weather_map_uniforms* u, int off) {
	const weather_now* weather = weather_now_get();
	float scale = g_ready && !off ? 1.0f / WEATHER_MAP_SPAN : 0.0f;

	glActiveTexture(GL_TEXTURE0 + WEATHER_MAP_UNIT);
	glBindTexture(GL_TEXTURE_2D, g_ready ? g_texture : 0);
	glActiveTexture(GL_TEXTURE0);
	glUniform1i(u->texture, WEATHER_MAP_UNIT);
	glUniform2f(u->origin, g_origin[0], g_origin[1]);
	glUniform1f(u->scale, scale);
	// With no map, the weather at the player is the weather everywhere; with
	// a flat view, none
	if(off)
		glUniform4f(u->global, 0.0f, 0.0f, 0.0f, 0.0f);
	else
		glUniform4f(u->global, weather->cloud, weather->rain, weather->snow, weather->storm);
}
