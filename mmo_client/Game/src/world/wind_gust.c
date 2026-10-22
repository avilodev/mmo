// Bands of stronger wind sweeping along the wind, and what they do to hair.

#include "world/wind_gust.h"
#include "world/weather_cells.h"

#include <math.h>

#define TWO_PI 6.283185307179586

// The second wave starts this far along, in radians
#define PHASE_B 1.7

// The sum of the waves between these is a lull up to a gust
#define GUST_LOW 0.25
#define GUST_HIGH 0.85

// Numbers into GLSL, so the shader and this file agree
#define STR_(x) #x
#define STR(x) STR_(x)

static const char* GLSL =
	"const float GUST_LEN_A = " STR(WIND_GUST_LENGTH_A) ";\n"
	"const float GUST_SPEED_A = " STR(WIND_GUST_SPEED_A) ";\n"
	"const float GUST_LEN_B = " STR(WIND_GUST_LENGTH_B) ";\n"
	"const float GUST_SPEED_B = " STR(WIND_GUST_SPEED_B) ";\n"
	"float wind_gust(vec2 xz, vec2 dir, float t) {\n"
	"    float d = dot(xz, dir);\n"
	"    float ca = (d - GUST_SPEED_A * t) / GUST_LEN_A;\n"
	"    float cb = (d - GUST_SPEED_B * t) / GUST_LEN_B;\n"
	"    float a = sin(6.2831853 * (ca - floor(ca)));\n"
	"    float b = sin(6.2831853 * (cb - floor(cb)) + " STR(PHASE_B) ");\n"
	"    float v = 0.5 + 0.5 * (0.6 * a + 0.4 * b);\n"
	"    return smoothstep(" STR(GUST_LOW) ", " STR(GUST_HIGH) ", v);\n"
	"}\n";

static double wave(double d, double t, double length, double speed, double phase) {
	double cycles = (d - speed * t) / length;

	return sin(TWO_PI * (cycles - floor(cycles)) + phase);
}

static double smooth_step(double lo, double hi, double x) {
	double t = (x - lo) / (hi - lo);

	if(t < 0.0)
		t = 0.0;
	if(t > 1.0)
		t = 1.0;

	return t * t * (3.0 - 2.0 * t);
}

void wind_direction(const weather_now* weather, float out[2]) {
	float length = sqrtf(weather->wind[0] * weather->wind[0] + weather->wind[1] * weather->wind[1]);

	if(length < 1e-3f) {
		// Nothing blowing yet: the way the weather goes
		weather_cells_drift(&out[0], &out[1], NULL);

		return;
	}
	out[0] = weather->wind[0] / length;
	out[1] = weather->wind[1] / length;
}

float wind_gust_at(const float dir[2], float time_s, float x, float y) {
	double d = (double)x * dir[0] + (double)y * dir[1];
	double a = wave(d, time_s, WIND_GUST_LENGTH_A, WIND_GUST_SPEED_A, 0.0);
	double b = wave(d, time_s, WIND_GUST_LENGTH_B, WIND_GUST_SPEED_B, PHASE_B);

	return (float)smooth_step(GUST_LOW, GUST_HIGH, 0.5 + 0.5 * (0.6 * a + 0.4 * b));
}

void wind_lean_at(const weather_now* weather, float x, float y, float out[3]) {
	float dir[2];
	float level = fminf(fmaxf(weather->wind_level, 0.0f), 3.0f);
	float gust;
	float lean;

	wind_direction(weather, dir);
	gust = wind_gust_at(dir, weather->time_s, x, y);
	lean = WIND_LEAN_PER_LEVEL_SQ * level * level *
	       (WIND_LEAN_LULL + WIND_LEAN_GUST_SPAN * gust);
	out[0] = dir[0] * lean;
	out[1] = lean * WIND_LEAN_LIFT;
	out[2] = dir[1] * lean;
}

const char* wind_gust_glsl(void) {
	return GLSL;
}
