// The lighting shared by the city and the characters.

#include "render/scene_light.h"
#include "render/shadow_pass.h"
#include "render/lamp_shadow.h"
#include "world/lamp_shadow_math.h"
#include "world/world_lighting.h"
#include "world/weather_now.h"
#include "world/weather_cells.h"
#include "world/weather_state.h"
#include "world/sky.h"
#include "render/weather_map.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#ifndef GL_TEXTURE1
#define GL_TEXTURE1 (GL_TEXTURE0 + 1)
#endif

#define STR_INNER(x) #x
#define STR(x) STR_INNER(x)

// How much of the cloud's shade over a spot shows: 1 all of it, 0 none
#define SCENE_CLOUD_SHADE 1.0
// A cloud's size, world units, and how many cells the noise repeats after
#define SCENE_CLOUD_CELL 420.0
#define SCENE_CLOUD_PERIOD 1024.0

// The light a surface receives. The big light is half-Lambert, since the
// camera looks down and a plain Lambert term leaves every side facing away
// from it flat. Shadows only take away the big light, never the ambient.
static const char* GLSL_HEAD =
	"uniform vec3 u_light_dir;\n"
	"uniform vec3 u_light_color;\n"
	"uniform vec3 u_ambient_up;\n"
	"uniform vec3 u_ambient_down;\n"
	"uniform float u_shadow_strength;\n"
	"uniform vec2 u_shadow_bias;\n"
	"uniform vec2 u_shadow_offset;\n"
	"uniform sampler2DShadow u_shadow_map0;\n"
	"uniform sampler2DShadow u_shadow_map1;\n"
	"uniform mat4 u_shadow_mat0;\n"
	"uniform mat4 u_shadow_mat1;\n"
	"uniform int u_lamp_count;\n"
	"uniform sampler2DShadow u_lamp_shadow;\n"
	"uniform int u_lamp_shadowed;\n"
	"uniform vec3 u_lamp_pos[" STR(SCENE_LIGHT_MAX_LAMPS) "];\n"
	"uniform float u_lamp_radius[" STR(SCENE_LIGHT_MAX_LAMPS) "];\n"
	"uniform vec3 u_lamp_color[" STR(SCENE_LIGHT_MAX_LAMPS) "];\n"
	"uniform vec3 u_eye;\n"
	"uniform vec3 u_fog_color;\n"
	"uniform float u_fog_density;\n"
	"uniform vec3 u_weather;\n"
	"uniform float u_weather_time;\n"
	"uniform vec2 u_cloud_drift;\n"
	"const float SHADOW_TEXEL = 1.0 / " STR(SHADOW_MAP_SIZE) ".0;\n"
	"\n"
	"float shadow_soft0(vec3 p) {\n"
	"    float sum = 0.0;\n"
	"    for (int x = -1; x <= 1; x++)\n"
	"        for (int y = -1; y <= 1; y++)\n"
	"            sum += texture(u_shadow_map0, vec3(p.xy + vec2(x, y) * SHADOW_TEXEL, p.z - u_shadow_bias.x));\n"
	"    return sum / 9.0;\n"
	"}\n"
	"\n"
	"float shadow_soft1(vec3 p) {\n"
	"    float sum = 0.0;\n"
	"    for (int x = -1; x <= 1; x++)\n"
	"        for (int y = -1; y <= 1; y++)\n"
	"            sum += texture(u_shadow_map1, vec3(p.xy + vec2(x, y) * SHADOW_TEXEL, p.z - u_shadow_bias.y));\n"
	"    return sum / 9.0;\n"
	"}\n"
	"\n"
	"// 1 in the light, 0 in shadow. The close map fades into the wide one at its edge\n"
	"float shadow_visibility(vec3 wpos, vec3 n) {\n"
	"    if (u_shadow_strength <= 0.0) return 1.0;\n"
	"    vec3 p0 = (u_shadow_mat0 * vec4(wpos + n * u_shadow_offset.x, 1.0)).xyz;\n"
	"    float e0 = max(abs(p0.x - 0.5), abs(p0.y - 0.5));\n"
	"    float v = 1.0;\n"
	"    if (e0 < 0.48) v = shadow_soft0(p0);\n"
	"    if (e0 > 0.38) {\n"
	"        vec3 p1 = (u_shadow_mat1 * vec4(wpos + n * u_shadow_offset.y, 1.0)).xyz;\n"
	"        float e1 = max(abs(p1.x - 0.5), abs(p1.y - 0.5));\n"
	"        float v1 = 1.0;\n"
	"        if (e1 < 0.48) v1 = shadow_soft1(p1);\n"
	"        v1 = mix(v1, 1.0, smoothstep(0.38, 0.48, e1));\n"
	"        v = mix(v, v1, smoothstep(0.38, 0.48, e0));\n"
	"    }\n"
	"    return mix(1.0, v, u_shadow_strength);\n"
	"}\n"
	"\n";


// What rain and snow do to the ground, and the fog they bring. The ground
// darkens and takes puddles and rings while it is wet, and goes white where it
// faces up while snow lies. scene_light() is not touched: these change the
// surface colour going in and the lit colour coming out.
static const char* GLSL_WEATHER =
	"const float RIPPLE_CELL = 18.0;\n"
	"\n"
	"float wx_hash(vec2 p) {\n"
	"    p = fract(p * vec2(123.34, 456.21));\n"
	"    p += dot(p, p + 45.32);\n"
	"    return fract(p.x * p.y);\n"
	"}\n"
	"\n"
	"float wx_noise(vec2 p) {\n"
	"    vec2 i = floor(p);\n"
	"    vec2 f = fract(p);\n"
	"    f = f * f * (3.0 - 2.0 * f);\n"
	"    return mix(mix(wx_hash(i), wx_hash(i + vec2(1.0, 0.0)), f.x),\n"
	"               mix(wx_hash(i + vec2(0.0, 1.0)), wx_hash(i + vec2(1.0, 1.0)), f.x), f.y);\n"
	"}\n"
	"\n"
	"// A ring spreading from a random point in each cell of the ground\n"
	"float wx_ripples(vec2 p, float t) {\n"
	"    vec2 cell = floor(p / RIPPLE_CELL);\n"
	"    vec2 local = p / RIPPLE_CELL - cell - 0.5;\n"
	"    float age = fract(t * 0.9 + wx_hash(cell));\n"
	"    vec2 centre = (vec2(wx_hash(cell + 7.0), wx_hash(cell + 13.0)) - 0.5) * 0.3;\n"
	"    float ring = abs(length(local - centre) - age * 0.3);\n"
	"    return (1.0 - smoothstep(0.0, 0.03, ring)) * (1.0 - age);\n"
	"}\n"
	"\n"
	"// Whether a roof or an awning is over this spot, 0 in the open, 1 well under\n"
	"float wx_under(vec3 wpos) {\n"
	"    return smoothstep(18.0, 36.0, cover_height(wpos.xz) - wpos.y);\n"
	"}\n"
	"\n"
	"// How much of the ground is a puddle here: pooled in the low patches\n"
	"float wx_puddle(vec3 n, vec3 wpos, float wet) {\n"
	"    float lumps = wx_noise(wpos.xz * 0.012) * 0.7 + wx_noise(wpos.xz * 0.05) * 0.3;\n"
	"    return smoothstep(0.57, 0.73, lumps) * smoothstep(0.55, 0.9, n.y) * smoothstep(0.35, 0.8, wet);\n"
	"}\n"
	"\n"
	"vec3 scene_surface(vec3 albedo, vec3 n, vec3 wpos) {\n"
	"    float dry = u_weather.x > 0.0 || u_weather.y > 0.0 ? 1.0 - wx_under(wpos) : 1.0;\n"
	"    float wet = u_weather.x * dry;\n"
	"    float snow = u_weather.y * dry;\n"
	"    float up = smoothstep(0.55, 0.9, n.y);\n"
	"    if (wet > 0.0) albedo *= 1.0 - wet * (0.30 + 0.10 * wx_puddle(n, wpos, wet));\n"
	"    if (snow > 0.0) {\n"
	"        float grain = wx_noise(wpos.xz * 0.2);\n"
	"        float cover = up * smoothstep(0.15, 0.55, snow * 1.5 - grain * 0.45);\n"
	"        albedo = mix(albedo, vec3(0.93, 0.95, 1.0), cover * 0.92);\n"
	"    }\n"
	"    return albedo;\n"
	"}\n"
	"\n"
	"// What wet ground gives back after the light: the sky in its puddles, and rings\n"
	"vec3 scene_sheen(vec3 n, vec3 wpos) {\n"
	"    if (u_weather.x <= 0.0) return vec3(0.0);\n"
	"    float wet = u_weather.x * (1.0 - wx_under(wpos));\n"
	"    float puddle = wx_puddle(n, wpos, wet);\n"
	"    float rings = wx_ripples(wpos.xz, u_weather_time) * smoothstep(0.55, 0.9, n.y) * u_weather.z;\n"
	"    return u_fog_color * (puddle * 0.45 + rings * wet * (0.15 + 0.35 * puddle));\n"
	"}\n"
	"\n"
	"// The air is thick where cloud, rain or snow is over the ground, so a patch of\n"
	"// weather seen across the plaza is a veil, and the clear gap beside it is not.\n"
	"// u_fog_density is 1 to have it, 0 to not.\n"
	"vec3 scene_fog(vec3 lit, vec3 wpos) {\n"
	"    if (u_fog_density <= 0.0) return lit;\n"
	"    vec4 w = weather_at(wpos.xz);\n"
	"    float density = smoothstep(0.5, 1.0, w.x) * 0.00006 + w.y * 0.00022 + w.z * 0.00040 + w.w * 0.00008;\n"
	"    float thick = 1.0 - exp(-density * length(wpos - u_eye));\n"
	"    return mix(lit, u_fog_color, thick);\n"
	"}\n";

static int g_flat;
static float g_eye[3];

// A lamp's shadow, from the face that sees the surface, the same lookup as
// lamp_shadow_locate(). 1 in the light, 0 in shadow.
static const char* GLSL_LAMP =
	"float lamp_visibility(int i, vec3 wpos, vec3 n) {\n"
	"    vec3 to = wpos - u_lamp_pos[i];\n"
	"    float dist = length(to);\n"
	"    vec3 v = to + n * (dist * 0.01 + 0.2);\n"
	"    vec3 a = abs(v);\n"
	"    int face = (a.x >= a.y && a.x >= a.z) ? (v.x > 0.0 ? 0 : 1)\n"
	"             : ((a.y >= a.z) ? (v.y > 0.0 ? 2 : 3) : (v.z > 0.0 ? 4 : 5));\n"
	"    vec3 d = LAMP_FACE_DIR[face];\n"
	"    vec3 up = LAMP_FACE_UP[face];\n"
	"    float along = dot(v, d);\n"
	"    if (along <= LAMP_NEAR) return 1.0;\n"
	"    vec2 ndc = vec2(dot(v, cross(d, up)), dot(v, up)) / along / (1.0 + LAMP_MARGIN);\n"
	"    float far_plane = u_lamp_radius[i];\n"
	"    float biased = along - (along * 0.01 + 0.25);\n"
	"    float depth = (far_plane + LAMP_NEAR) / (far_plane - LAMP_NEAR)\n"
	"                - 2.0 * far_plane * LAMP_NEAR / ((far_plane - LAMP_NEAR) * biased);\n"
	"    vec2 uv = (vec2(float(face), float(i)) + ndc * 0.5 + 0.5) / vec2(LAMP_COLS, LAMP_ROWS);\n"
	"    float sum = 0.0;\n"
	"    for (int x = -1; x <= 1; x++)\n"
	"        for (int y = -1; y <= 1; y++)\n"
	"            sum += texture(u_lamp_shadow, vec3(uv + vec2(x, y) * LAMP_TEXEL, depth * 0.5 + 0.5));\n"
	"    return sum / 9.0;\n"
	"}\n"
	"\n"
	"// Cloud shadows. Each cloud is a lump of noise that drifts with the weather; where the\n"
	"// weather map says it is cloudier, more of the ground is under one. The sun is dimmed for\n"
	"// the cloud over the camera already, so a spot is lit against the average cloud there:\n"
	"// the gap between clouds is bright, the shade under one dark, and the mean stays put.\n"
	"const float OVERCAST_SUN = " STR(WORLD_LIGHTING_OVERCAST_SUN) ";\n"
	"const float CLOUD_SHADE = " STR(SCENE_CLOUD_SHADE) ";\n"
	"const float CS_CELL = " STR(SCENE_CLOUD_CELL) ";\n"
	"const float CS_PERIOD = " STR(SCENE_CLOUD_PERIOD) ";\n"
	"\n"
	"float cs_hash(vec2 i, float period) {\n"
	"    vec3 p3 = fract(vec3(mod(i, period).xyx) * 0.1031);\n"
	"    p3 += dot(p3, p3.yzx + 33.33);\n"
	"    return fract((p3.x + p3.y) * p3.z);\n"
	"}\n"
	"\n"
	"float cs_noise(vec2 p, float period) {\n"
	"    vec2 i = floor(p);\n"
	"    vec2 f = fract(p);\n"
	"    f = f * f * (3.0 - 2.0 * f);\n"
	"    return mix(mix(cs_hash(i, period), cs_hash(i + vec2(1.0, 0.0), period), f.x),\n"
	"               mix(cs_hash(i + vec2(0.0, 1.0), period), cs_hash(i + vec2(1.0, 1.0), period), f.x), f.y);\n"
	"}\n"
	"\n"
	"// 0 to 1, spread evenly, the same everywhere at once and repeating far out\n"
	"float cloud_detail(vec2 xz) {\n"
	"    vec2 q = (xz - u_cloud_drift) / CS_CELL;\n"
	"    float n = 0.65 * cs_noise(q, CS_PERIOD) + 0.35 * cs_noise(q * 2.0, CS_PERIOD * 2.0);\n"
	"    return smoothstep(0.25, 0.75, n);\n"
	"}\n"
	"\n"
	"// How much of the ground a cloud cover of c shades\n"
	"float cloud_cover_of(float c) {\n"
	"    return clamp(mix(-0.2, 1.2, c), 0.0, 1.0);\n"
	"}\n"
	"\n"
	"float cloud_shade(vec3 wpos) {\n"
	"    if (CLOUD_SHADE <= 0.0 || u_wmap_scale <= 0.0) return 1.0;\n"
	"    float here = weather_at(wpos.xz).x;\n"
	"    float centre = weather_at(u_wmap_origin + 0.5 / u_wmap_scale).x;\n"
	"    float edge = mix(-0.2, 1.2, here);\n"
	"    float under = smoothstep(edge + 0.08, edge - 0.08, cloud_detail(wpos.xz));\n"
	"    float sun = 1.0 - (1.0 - OVERCAST_SUN) * under;\n"
	"    float mean = 1.0 - (1.0 - OVERCAST_SUN) * cloud_cover_of(centre);\n"
	"    return mix(1.0, sun / mean, CLOUD_SHADE);\n"
	"}\n"
	"\n"
	"vec3 scene_light(vec3 n, vec3 wpos) {\n"
	"    float wrap = dot(n, u_light_dir) * 0.5 + 0.5;\n"
	"    vec3 ambient = mix(u_ambient_down, u_ambient_up, n.y * 0.5 + 0.5);\n"
	"    vec3 lit = ambient + u_light_color * (wrap * wrap) * shadow_visibility(wpos, n) * cloud_shade(wpos);\n"
	"    for (int i = 0; i < u_lamp_count; i++) {\n"
	"        vec3 to_lamp = u_lamp_pos[i] - wpos;\n"
	"        float dist = length(to_lamp);\n"
	"        float reach = clamp(1.0 - dist / u_lamp_radius[i], 0.0, 1.0);\n"
	"        float facing = max(dot(n, to_lamp / max(dist, 0.001)), 0.0);\n"
	"        float k = reach * reach * facing;\n"
	"        if (k > 0.0 && u_lamp_shadowed != 0) k *= lamp_visibility(i, wpos, n);\n"
	"        lit += u_lamp_color[i] * k;\n"
	"    }\n"
	"    return lit;\n"
	"}\n";

// The whole GLSL text: the head, the face table made from the same data the C
// side draws with, and the lamp lookup
static char g_glsl[16384];

static int append_vec3_table(char* out, int size, const char* name,
                             const float* (*row)(int)) {
	int used = snprintf(out, size, "const vec3 %s[%d] = vec3[%d](", name, LAMP_SHADOW_FACES,
	                    LAMP_SHADOW_FACES);

	for(int f = 0; f < LAMP_SHADOW_FACES; f++)
		used += snprintf(out + used, size - used, "%svec3(%.1f, %.1f, %.1f)", f ? ", " : "",
		                 row(f)[0], row(f)[1], row(f)[2]);
	used += snprintf(out + used, size - used, ");\n");

	return used;
}

const char* scene_light_glsl(void) {
	int used;

	if(g_glsl[0])
		return g_glsl;
	used = snprintf(g_glsl, sizeof(g_glsl), "%s%s%s", cover_map_glsl(), weather_map_glsl(),
	                GLSL_HEAD);
	used += snprintf(g_glsl + used, sizeof(g_glsl) - used,
	                 "const float LAMP_NEAR = %.1f;\n"
	                 "const float LAMP_MARGIN = %.4f;\n"
	                 "const float LAMP_COLS = %d.0;\n"
	                 "const float LAMP_ROWS = %d.0;\n"
	                 "const vec2 LAMP_TEXEL = vec2(1.0 / %d.0, 1.0 / %d.0);\n",
	                 LAMP_SHADOW_NEAR, LAMP_SHADOW_MARGIN, LAMP_SHADOW_FACES,
	                 LAMP_SHADOW_MAX_LAMPS, LAMP_SHADOW_TILE * LAMP_SHADOW_FACES,
	                 LAMP_SHADOW_TILE * LAMP_SHADOW_MAX_LAMPS);
	used += append_vec3_table(g_glsl + used, (int)sizeof(g_glsl) - used, "LAMP_FACE_DIR",
	                          lamp_shadow_face_dir);
	used += append_vec3_table(g_glsl + used, (int)sizeof(g_glsl) - used, "LAMP_FACE_UP",
	                          lamp_shadow_face_up);
	used += snprintf(g_glsl + used, sizeof(g_glsl) - used, "%s", GLSL_LAMP);
	snprintf(g_glsl + used, sizeof(g_glsl) - used, "%s", GLSL_WEATHER);

	return g_glsl;
}

static GLint locate_indexed(GLuint program, const char* name, int index) {
	char full[48];

	snprintf(full, sizeof(full), "%s[%d]", name, index);

	return glGetUniformLocation(program, full);
}

static GLint locate_numbered(GLuint program, const char* name, int number) {
	char full[48];

	snprintf(full, sizeof(full), "%s%d", name, number);

	return glGetUniformLocation(program, full);
}

void scene_light_locate(GLuint program, scene_light_uniforms* out) {
	out->light_dir = glGetUniformLocation(program, "u_light_dir");
	out->light_color = glGetUniformLocation(program, "u_light_color");
	out->ambient_up = glGetUniformLocation(program, "u_ambient_up");
	out->ambient_down = glGetUniformLocation(program, "u_ambient_down");
	out->shadow_strength = glGetUniformLocation(program, "u_shadow_strength");
	out->shadow_bias = glGetUniformLocation(program, "u_shadow_bias");
	out->shadow_offset = glGetUniformLocation(program, "u_shadow_offset");
	for(int c = 0; c < SHADOW_CASCADES; c++) {
		out->shadow_map[c] = locate_numbered(program, "u_shadow_map", c);
		out->shadow_mat[c] = locate_numbered(program, "u_shadow_mat", c);
	}
	out->lamp_count = glGetUniformLocation(program, "u_lamp_count");
	out->lamp_shadow = glGetUniformLocation(program, "u_lamp_shadow");
	out->lamp_shadowed = glGetUniformLocation(program, "u_lamp_shadowed");
	for(int i = 0; i < SCENE_LIGHT_MAX_LAMPS; i++) {
		out->lamp_pos[i] = locate_indexed(program, "u_lamp_pos", i);
		out->lamp_radius[i] = locate_indexed(program, "u_lamp_radius", i);
		out->lamp_color[i] = locate_indexed(program, "u_lamp_color", i);
	}
	out->eye = glGetUniformLocation(program, "u_eye");
	out->fog_color = glGetUniformLocation(program, "u_fog_color");
	out->fog_density = glGetUniformLocation(program, "u_fog_density");
	out->weather = glGetUniformLocation(program, "u_weather");
	out->weather_time = glGetUniformLocation(program, "u_weather_time");
	out->cloud_drift = glGetUniformLocation(program, "u_cloud_drift");
	cover_map_locate(program, &out->cover);
	weather_map_locate(program, &out->map);
}

static void apply_shadows(const scene_light_uniforms* u, int on, float strength) {
	float bias[SHADOW_CASCADES] = { 0 };
	float offset[SHADOW_CASCADES] = { 0 };

	for(int c = 0; c < SHADOW_CASCADES; c++) {
		glActiveTexture(GL_TEXTURE1 + c);
		glBindTexture(GL_TEXTURE_2D, shadow_map_texture(c));
		glUniform1i(u->shadow_map[c], 1 + c);
		if(on) {
			const shadow_cascade* cascade = shadow_pass_cascade(c);

			glUniformMatrix4fv(u->shadow_mat[c], 1, GL_FALSE, cascade->tex);
			// A couple of texels of slack, on the depth and along the normal
			bias[c] = 2.0f * cascade->texel / cascade->depth_range;
			offset[c] = 1.5f * cascade->texel;
		}
	}
	glActiveTexture(GL_TEXTURE0);
	glUniform1f(u->shadow_strength, on ? strength : 0.0f);
	glUniform2f(u->shadow_bias, bias[0], bias[1]);
	glUniform2f(u->shadow_offset, offset[0], offset[1]);
}

static void apply_lamps(const scene_light_uniforms* u, float amount) {
	const city_lamp* lamps = NULL;
	int count = 0;
	int shadowed = 0;

	if(amount > 0.01f)
		count = lamp_shadow_lamps(&lamps);
	shadowed = count > 0 && lamp_shadow_active();
	glUniform1i(u->lamp_count, count);
	glUniform1i(u->lamp_shadowed, shadowed);
	// The lamps' shadows are on texture unit 3
	glActiveTexture(GL_TEXTURE0 + 3);
	glBindTexture(GL_TEXTURE_2D, lamp_shadow_texture());
	glActiveTexture(GL_TEXTURE0);
	glUniform1i(u->lamp_shadow, 3);
	for(int i = 0; i < count; i++) {
		glUniform3f(u->lamp_pos[i], lamps[i].pos[0], lamps[i].pos[1], lamps[i].pos[2]);
		glUniform1f(u->lamp_radius[i], lamps[i].radius);
		glUniform3f(u->lamp_color[i], lamps[i].color[0] * amount, lamps[i].color[1] * amount,
		            lamps[i].color[2] * amount);
	}
}

// The fog and the wet ground: none in the map and the previews
static void apply_weather(const scene_light_uniforms* u) {
	const weather_now* weather = weather_now_get();
	const sky_state* sky = sky_current();
	// The fog is the weather map's now; here it is only on or off
	float fog = g_flat ? 0.0f : 1.0f;
	float wet = g_flat ? 0.0f : weather->wet;
	float snow = g_flat ? 0.0f : weather->snow_cover;
	float rain = g_flat ? 0.0f : weather->rain;

	glUniform3f(u->eye, g_eye[0], g_eye[1], g_eye[2]);
	glUniform3f(u->fog_color, sky->haze[0], sky->haze[1], sky->haze[2]);
	glUniform1f(u->fog_density, fog);
	glUniform3f(u->weather, wet, snow, rain);
	glUniform1f(u->weather_time, weather->time_s);
	{
		// The clouds drift the way and as fast as the weather's patches, kept in
		// double and wrapped where the noise repeats, so they never jump
		float dir[2];
		float speed;
		double shared = weather_state_shared_now_s();
		double period = SCENE_CLOUD_CELL * SCENE_CLOUD_PERIOD;

		weather_cells_drift(&dir[0], &dir[1], &speed);
		glUniform2f(u->cloud_drift, (float)fmod((double)dir[0] * (double)speed * shared, period),
		            (float)fmod((double)dir[1] * (double)speed * shared, period));
	}
	cover_map_apply(&u->cover);
	weather_map_apply(&u->map, g_flat);
}

void scene_light_apply(const scene_light_uniforms* u) {
	world_lighting noon;
	const world_lighting* light = world_lighting_current();

	if(g_flat) {
		world_lighting_noon(&noon);
		light = &noon;
	}
	glUniform3f(u->light_dir, light->dir[0], light->dir[1], light->dir[2]);
	glUniform3f(u->light_color, light->color[0], light->color[1], light->color[2]);
	glUniform3f(u->ambient_up, light->ambient_up[0], light->ambient_up[1], light->ambient_up[2]);
	glUniform3f(u->ambient_down, light->ambient_down[0], light->ambient_down[1],
	            light->ambient_down[2]);
	apply_shadows(u, !g_flat && shadow_pass_active(), light->shadow_strength);
	apply_lamps(u, g_flat ? 0.0f : light->lamp_amount);
	apply_weather(u);
}

void scene_light_set_eye(const float eye[3]) {
	memcpy(g_eye, eye, sizeof(g_eye));
}

void scene_light_set_flat(int flat) {
	g_flat = flat;
}
