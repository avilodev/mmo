// Check the gusts: bands of wind that come and go, wrap with the shared clock,
// and lean hair along the wind by how hard it blows.
#include "world/wind_gust.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static int g_failures;

#define CHECK(cond, msg) \
	do { \
		if(cond) \
			printf("  ok   %s\n", msg); \
		else { \
			printf("  FAIL %s\n", msg); \
			g_failures++; \
		} \
	} while(0)

static weather_now blowing(float level, float x, float y) {
	weather_now w;

	memset(&w, 0, sizeof(w));
	w.wind_level = level;
	w.wind[0] = x;
	w.wind[1] = y;
	w.time_s = 123.0f;

	return w;
}

static float length_of(const float v[3]) {
	return sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}

static void test_gust(void) {
	const float dir[2] = { 0.6f, 0.8f };
	float low = 1.0f;
	float high = 0.0f;
	float sum = 0.0f;
	int count = 0;
	int wrapped = 1;

	printf("\nTEST 1: gusts are bands that come and go over the ground\n");
	for(int i = 0; i < 400; i++) {
		float g = wind_gust_at(dir, 500.0f, (float)i * 17.0f, (float)i * 9.0f);

		low = fminf(low, g);
		high = fmaxf(high, g);
		sum += g;
		count++;
	}
	CHECK(low >= 0.0f && high <= 1.0f, "a gust is always between 0 and 1");
	CHECK(low < 0.05f, "there are lulls");
	CHECK(high > 0.95f, "there are full gusts");
	CHECK(sum / (float)count > 0.25f && sum / (float)count < 0.75f,
	      "about half the ground is in a gust at a moment");

	for(int i = 0; i < 50; i++) {
		float x = (float)i * 131.0f;
		float y = (float)i * 57.0f;

		if(fabsf(wind_gust_at(dir, 0.0f, x, y) - wind_gust_at(dir, 16384.0f, x, y)) > 1e-4f)
			wrapped = 0;
	}
	CHECK(wrapped, "the bands are the same when the shared clock wraps");
	CHECK(wind_gust_at(dir, 10.0f, 300.0f, 200.0f) != wind_gust_at(dir, 14.0f, 300.0f, 200.0f),
	      "a spot goes in and out of gusts as time passes");
}

static void test_lean(void) {
	float calm[3], breezy[3], windy[3], gale[3];
	float east_calm[3];
	weather_now w;

	printf("\nTEST 2: hair leans along the wind, more as it blows harder\n");
	w = blowing(0.0f, 100.0f, 0.0f);
	wind_lean_at(&w, 50.0f, 50.0f, east_calm);
	CHECK(length_of(east_calm) < 1e-6f, "no lean in calm");

	// The same spot and moment for every level, so only the level differs
	w = blowing(1.0f, 100.0f, 0.0f);
	wind_lean_at(&w, 50.0f, 50.0f, breezy);
	w = blowing(2.0f, 100.0f, 0.0f);
	wind_lean_at(&w, 50.0f, 50.0f, windy);
	w = blowing(3.0f, 100.0f, 0.0f);
	wind_lean_at(&w, 50.0f, 50.0f, gale);
	CHECK(length_of(breezy) > 0.0f, "a breeze leans hair");
	CHECK(length_of(windy) > length_of(breezy) * 3.0f, "windy leans it much more than a breeze");
	CHECK(length_of(gale) > length_of(windy) * 1.8f, "a gale leans it much more than windy");
	CHECK(gale[0] > 0.0f && fabsf(gale[2]) < 1e-6f, "it leans along the wind (east)");
	CHECK(gale[1] > 0.0f && gale[1] < gale[0], "it lifts a little, less than it leans");

	w = blowing(3.0f, 0.0f, -50.0f);
	wind_lean_at(&w, 50.0f, 50.0f, calm);
	CHECK(calm[2] < 0.0f && fabsf(calm[0]) < 1e-6f, "a wind along -y leans it along -z");
}

static void test_direction(void) {
	float dir[2];
	weather_now w = blowing(2.0f, 3.0f, 4.0f);

	printf("\nTEST 3: the wind's direction is a unit vector\n");
	wind_direction(&w, dir);
	CHECK(fabsf(dir[0] - 0.6f) < 1e-5f && fabsf(dir[1] - 0.8f) < 1e-5f, "3,4 is 0.6,0.8");
	w = blowing(0.0f, 0.0f, 0.0f);
	wind_direction(&w, dir);
	CHECK(fabsf(sqrtf(dir[0] * dir[0] + dir[1] * dir[1]) - 1.0f) < 1e-4f,
	      "with nothing blowing it is still a unit vector (the way weather travels)");
	CHECK(strstr(wind_gust_glsl(), "float wind_gust(vec2 xz, vec2 dir, float t)") != NULL,
	      "the shader gets the same function");
}

int main(void) {
	printf("=== wind gust ===\n");
	test_gust();
	test_lean();
	test_direction();
	printf("\n%s (%d failures)\n", g_failures ? "FAILED" : "ALL PASSED", g_failures);

	return g_failures ? 1 : 0;
}
