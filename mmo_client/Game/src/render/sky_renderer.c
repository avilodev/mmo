// Compile the sky shader and paint the sky behind the world.

#include "render/gl_loader.h"
#include "render/sky_renderer.h"
#include "core/client_log.h"
#include "world/weather_now.h"
#include "render/weather_map.h"
#include "camera/camera_math.h"

static const char* VERTEX_SRC =
	"#version 330 core\n"
	"out vec2 v_ndc;\n"
	"void main() {\n"
	"    vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);\n"
	"    v_ndc = p * 2.0 - 1.0;\n"
	"    gl_Position = vec4(v_ndc, 0.0, 1.0);\n"
	"}\n";

static const char* FRAGMENT_VERSION = "#version 330 core\n";

static const char* FRAGMENT_SRC =
	"in vec2 v_ndc;\n"
	"uniform mat4 u_inv_viewproj;\n"
	"uniform vec3 u_zenith;\n"
	"uniform vec3 u_horizon;\n"
	"uniform vec3 u_haze;\n"
	"uniform vec3 u_sun_dir;\n"
	"uniform vec3 u_sun_color;\n"
	"uniform vec3 u_moon_dir;\n"
	"uniform float u_star_amount;\n"
	"uniform float u_cloud;\n"
	"uniform float u_storm;\n"
	"uniform float u_flash;\n"
	"uniform float u_cloud_time;\n"
	"uniform vec3 u_eye;\n"
	"uniform vec3 u_bolt;\n"
	"out vec4 frag;\n"
	"\n"
	"// How far up the sky thickens into haze, and how far the sun sinks into it\n"
	"const float HAZE_BAND = 0.06;\n"
	"const float SINK_BAND = 0.08;\n"
	"\n"
	"vec3 world_at(float z) {\n"
	"    vec4 w = u_inv_viewproj * vec4(v_ndc, z, 1.0);\n"
	"    return w.xyz / w.w;\n"
	"}\n"
	"\n"
	"float hash3(vec3 p) {\n"
	"    uvec3 q = uvec3(ivec3(floor(p)) + 100000);\n"
	"    q = q * 1664525u + 1013904223u;\n"
	"    q.x += q.y * q.z; q.y += q.z * q.x; q.z += q.x * q.y;\n"
	"    q ^= q >> 16u;\n"
	"    q.x += q.y * q.z; q.y += q.z * q.x; q.z += q.x * q.y;\n"
	"    return float(q.x ^ q.y ^ q.z) / 4294967295.0;\n"
	"}\n"
	"\n"
	"float hash2(vec2 p) {\n"
	"    p = fract(p * vec2(123.34, 456.21));\n"
	"    p += dot(p, p + 45.32);\n"
	"    return fract(p.x * p.y);\n"
	"}\n"
	"\n"
	"float noise2(vec2 p) {\n"
	"    vec2 i = floor(p);\n"
	"    vec2 f = fract(p);\n"
	"    f = f * f * (3.0 - 2.0 * f);\n"
	"    return mix(mix(hash2(i), hash2(i + vec2(1.0, 0.0)), f.x),\n"
	"               mix(hash2(i + vec2(0.0, 1.0)), hash2(i + vec2(1.0, 1.0)), f.x), f.y);\n"
	"}\n"
	"\n"
	"float fbm(vec2 p) {\n"
	"    float sum = 0.0;\n"
	"    float amp = 0.5;\n"
	"    for (int i = 0; i < 5; i++) {\n"
	"        sum += amp * noise2(p);\n"
	"        p = p * 2.03 + 17.1;\n"
	"        amp *= 0.5;\n"
	"    }\n"
	"    return sum;\n"
	"}\n"
	"\n"
	"// How high the cloud deck is, so a view ray can be followed to the ground under a cloud\n"
	"const float DECK_HEIGHT = 2800.0;\n"
	"\n"
	"// The cloud deck: flat, overhead, drifting steadily. x is how thick, y how lit.\n"
	"vec2 cloud_deck(vec3 dir, float cover) {\n"
	"    vec2 p = dir.xz / (dir.y + 0.15) * 2.2 + vec2(0.017, 0.009) * u_cloud_time;\n"
	"    float n = fbm(p);\n"
	"    float edge = mix(0.85, 0.2, cover);\n"
	"    float thick = smoothstep(edge, edge + 0.25, n);\n"
	"    return vec2(thick, smoothstep(edge, edge + 0.5, n));\n"
	"}\n"
	"\n"
	"// A jagged line from the clouds toward the ground, at the bolt's bearing\n"
	"float bolt_at(vec3 dir) {\n"
	"    if (u_bolt.y <= 0.0 || dir.y < 0.03) return 0.0;\n"
	"    float way = mod(atan(dir.z, dir.x) - u_bolt.x + 3.14159, 6.28318) - 3.14159;\n"
	"    float seed = u_bolt.z * 50.0;\n"
	"    float wander = (noise2(vec2(dir.y * 14.0, seed)) - 0.5) * 0.10\n"
	"                 + (noise2(vec2(dir.y * 60.0, seed + 9.0)) - 0.5) * 0.030\n"
	"                 + (noise2(vec2(dir.y * 210.0, seed + 21.0)) - 0.5) * 0.010;\n"
	"    float off = abs(way - wander);\n"
	"    float span = smoothstep(0.03, 0.10, dir.y) * (1.0 - smoothstep(0.55, 0.75, dir.y));\n"
	"    return ((1.0 - smoothstep(0.0, 0.0012, off)) + exp(-off * 40.0) * 0.45) * span * u_bolt.y;\n"
	"}\n"
	"\n"
	"void main() {\n"
	"    vec3 dir = normalize(world_at(1.0) - world_at(-1.0));\n"
	"    float up = max(dir.y, 0.0);\n"
	"    vec3 c = mix(u_horizon, u_zenith, pow(up, 0.6));\n"
	"\n"
	"    // The sky thickens into the haze at the horizon, so there is no line\n"
	"    c = mix(u_haze, c, smoothstep(0.0, HAZE_BAND, dir.y));\n"
	"\n"
	"    // A warm glow round the sun, fading out below the horizon\n"
	"    float to_sun = max(dot(dir, u_sun_dir), 0.0);\n"
	"    c += u_sun_color * pow(to_sun, 12.0) * 0.30 * (1.0 - 0.9 * u_cloud) * smoothstep(-0.15, 0.0, dir.y);\n"
	"\n"
	"    // Stars, where the sky is dark\n"
	"    vec3 cell = dir * 500.0;\n"
	"    float on = step(0.997, hash3(cell));\n"
	"    float round_star = 1.0 - smoothstep(0.15, 0.40, length(fract(cell) - 0.5));\n"
	"    c += vec3(1.0) * on * round_star * u_star_amount * smoothstep(0.02, 0.2, dir.y);\n"
	"\n"
	"    // The moon, then the sun on top; both sink into the haze as they set\n"
	"    float sink = smoothstep(0.0, SINK_BAND, dir.y);\n"
	"    float moon = 1.0 - smoothstep(0.9990, 0.9994, dot(dir, u_moon_dir));\n"
	"    c = mix(c, vec3(0.88, 0.92, 1.0), (1.0 - moon) * sink);\n"
	"    float sun = smoothstep(0.9985, 0.9990, dot(dir, u_sun_dir));\n"
	"    c = mix(c, mix(u_sun_color, vec3(1.0, 0.97, 0.90), 0.35), sun * sink);\n"
	"\n"
	"    // Clouds cover the stars, the moon and the sun, and fade into the haze. How\n"
	"    // thick they are depends on the weather over the ground they hang above.\n"
	"    if (dir.y > 0.0) {\n"
	"        vec4 over = weather_at(u_eye.xz + dir.xz / max(dir.y, 0.06) * DECK_HEIGHT);\n"
	"        float cover = over.x;\n"
	"        vec2 deck = cloud_deck(dir, cover);\n"
	"        float grey = dot(u_horizon, vec3(0.30, 0.59, 0.11));\n"
	"        vec3 body = mix(vec3(grey), u_horizon, 0.35) * 1.25;\n"
	"        float falling = max(over.y, over.z);\n"
	"        body *= (1.0 - 0.35 * deck.y) * (1.0 - 0.4 * over.w) * (1.0 - 0.22 * falling);\n"
	"        body += u_sun_color * pow(to_sun, 6.0) * 0.5 * (1.0 - deck.y) * (1.0 - 0.6 * cover);\n"
	"        body += vec3(0.50, 0.55, 0.80) * u_flash * (0.4 + 0.6 * deck.y);\n"
	"        if (cover > 0.05)\n"
	"            c = mix(c, body, deck.x * smoothstep(0.0, 0.12, dir.y));\n"
	"    }\n"
	"    c += vec3(0.85, 0.90, 1.0) * bolt_at(dir);\n"
	"\n"
	"    frag = vec4(c, 1.0);\n"
	"}\n";

static GLuint g_program;
static GLuint g_vao;
static GLint u_inv_viewproj, u_zenith, u_horizon, u_haze, u_sun_dir, u_sun_color, u_moon_dir;
static GLint u_star_amount, u_cloud, u_storm, u_flash, u_cloud_time, u_bolt, u_eye;
static weather_map_uniforms g_map;

static GLuint compile(GLenum type, const char* const* parts, int count) {
	GLuint shader = glCreateShader(type);
	GLint ok = 0;

	glShaderSource(shader, count, parts, NULL);
	glCompileShader(shader);
	glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
	if(!ok) {
		char log[1024];

		glGetShaderInfoLog(shader, (GLsizei)sizeof(log), NULL, log);
		CLOG_ERROR("[SKY] %s shader failed to compile: %s",
		           type == GL_VERTEX_SHADER ? "vertex" : "fragment", log);
		glDeleteShader(shader);
		return 0;
	}

	return shader;
}

static int link_program(GLuint vs, GLuint fs) {
	GLint ok = 0;

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
		CLOG_ERROR("[SKY] Shader program failed to link: %s", log);
		glDeleteProgram(g_program);
		g_program = 0;
		return 0;
	}

	return 1;
}

int sky_renderer_init(void) {
	const char* const vertex_parts[] = { VERTEX_SRC };
	const char* const fragment_parts[] = { FRAGMENT_VERSION, weather_map_glsl(), FRAGMENT_SRC };
	GLuint vs = compile(GL_VERTEX_SHADER, vertex_parts, 1);
	GLuint fs = compile(GL_FRAGMENT_SHADER, fragment_parts, 3);

	if(!vs || !fs) {
		if(vs)
			glDeleteShader(vs);
		if(fs)
			glDeleteShader(fs);
		return 0;
	}
	if(!link_program(vs, fs))
		return 0;
	u_inv_viewproj = glGetUniformLocation(g_program, "u_inv_viewproj");
	u_zenith = glGetUniformLocation(g_program, "u_zenith");
	u_horizon = glGetUniformLocation(g_program, "u_horizon");
	u_haze = glGetUniformLocation(g_program, "u_haze");
	u_sun_dir = glGetUniformLocation(g_program, "u_sun_dir");
	u_sun_color = glGetUniformLocation(g_program, "u_sun_color");
	u_moon_dir = glGetUniformLocation(g_program, "u_moon_dir");
	u_star_amount = glGetUniformLocation(g_program, "u_star_amount");
	u_cloud = glGetUniformLocation(g_program, "u_cloud");
	u_storm = glGetUniformLocation(g_program, "u_storm");
	u_flash = glGetUniformLocation(g_program, "u_flash");
	u_cloud_time = glGetUniformLocation(g_program, "u_cloud_time");
	u_bolt = glGetUniformLocation(g_program, "u_bolt");
	u_eye = glGetUniformLocation(g_program, "u_eye");
	weather_map_locate(g_program, &g_map);
	glGenVertexArrays(1, &g_vao);
	CLOG_INFO("[SKY] Shader ready");

	return 1;
}

void sky_renderer_shutdown(void) {
	if(g_vao) {
		glDeleteVertexArrays(1, &g_vao);
		g_vao = 0;
	}
	if(g_program) {
		glDeleteProgram(g_program);
		g_program = 0;
	}
}

void sky_renderer_draw(const CameraView* view, const sky_state* sky) {
	const weather_now* weather = weather_now_get();

	if(!g_program)
		return;

	glDisable(GL_DEPTH_TEST);
	glUseProgram(g_program);
	glBindVertexArray(g_vao);
	glUniformMatrix4fv(u_inv_viewproj, 1, GL_FALSE, view->inv_viewproj);
	glUniform3f(u_zenith, sky->zenith[0], sky->zenith[1], sky->zenith[2]);
	glUniform3f(u_horizon, sky->horizon[0], sky->horizon[1], sky->horizon[2]);
	glUniform3f(u_haze, sky->haze[0], sky->haze[1], sky->haze[2]);
	glUniform3f(u_sun_dir, sky->sun_dir[0], sky->sun_dir[1], sky->sun_dir[2]);
	glUniform3f(u_sun_color, sky->sun_color[0], sky->sun_color[1], sky->sun_color[2]);
	glUniform3f(u_moon_dir, sky->moon_dir[0], sky->moon_dir[1], sky->moon_dir[2]);
	glUniform1f(u_star_amount, sky->star_amount);
	glUniform1f(u_cloud, weather->cloud);
	glUniform1f(u_storm, weather->storm);
	glUniform1f(u_flash, weather->flash);
	glUniform1f(u_cloud_time, weather->cloud_time_s);
	glUniform3f(u_bolt, weather->bolt_angle, weather->bolt, weather->bolt_seed);
	{
		float eye[3];

		camera_view_eye(view, eye);
		glUniform3f(u_eye, eye[0], eye[1], eye[2]);
	}
	weather_map_apply(&g_map, 0);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	glBindVertexArray(0);
	glUseProgram(0);
}
