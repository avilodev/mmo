// Check the lamp shadow views: the shader's lookup must agree with the views drawn.
#include "world/lamp_shadow_math.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define NEAR_PLANE 8.0f
#define FAR_PLANE 240.0f
#define NEAR(a, b, eps) (fabsf((a) - (b)) < (eps))

static float rnd(void) {
	return (float)rand() / (float)RAND_MAX * 2.0f - 1.0f;
}

static void project(const float* m, const float* p, float* out_ndc) {
	float c[4];

	for(int row = 0; row < 4; row++)
		c[row] = m[row] * p[0] + m[4 + row] * p[1] + m[8 + row] * p[2] + m[12 + row];
	for(int i = 0; i < 3; i++)
		out_ndc[i] = c[i] / c[3];
}

static void test_faces_are_orthonormal(void) {
	for(int f = 0; f < LAMP_SHADOW_FACES; f++) {
		const float* d = lamp_shadow_face_dir(f);
		const float* u = lamp_shadow_face_up(f);

		assert(NEAR(d[0] * d[0] + d[1] * d[1] + d[2] * d[2], 1.0f, 1.0e-6f));
		assert(NEAR(u[0] * u[0] + u[1] * u[1] + u[2] * u[2], 1.0f, 1.0e-6f));
		assert(NEAR(d[0] * u[0] + d[1] * u[1] + d[2] * u[2], 0.0f, 1.0e-6f));
	}
}

static void test_the_six_faces_face_six_ways(void) {
	float pos[3] = { 100.0f, 50.0f, -30.0f };
	float vp[LAMP_SHADOW_FACES][16];

	lamp_shadow_fit(pos, NEAR_PLANE, FAR_PLANE, vp);
	for(int f = 0; f < LAMP_SHADOW_FACES; f++) {
		const float* d = lamp_shadow_face_dir(f);
		float ahead[3] = { pos[0] + d[0] * 100.0f, pos[1] + d[1] * 100.0f, pos[2] + d[2] * 100.0f };
		float ndc[3];

		project(vp[f], ahead, ndc);
		// Straight ahead is the middle of the face
		assert(NEAR(ndc[0], 0.0f, 1.0e-4f) && NEAR(ndc[1], 0.0f, 1.0e-4f));
		assert(ndc[2] > -1.0f && ndc[2] < 1.0f);
	}
}

static void test_the_lookup_agrees_with_the_views(void) {
	float pos[3] = { 12.0f, 80.0f, 33.0f };
	float vp[LAMP_SHADOW_FACES][16];

	srand(7);
	lamp_shadow_fit(pos, NEAR_PLANE, FAR_PLANE, vp);
	for(int i = 0; i < 5000; i++) {
		float v[3] = { rnd() * 200.0f, rnd() * 200.0f, rnd() * 200.0f };
		float p[3] = { pos[0] + v[0], pos[1] + v[1], pos[2] + v[2] };
		float x, y, dist, ndc[3];
		int face = lamp_shadow_locate(v, &x, &y, &dist);

		if(dist < NEAR_PLANE * 1.5f || dist > FAR_PLANE * 0.9f)
			continue;
		project(vp[face], p, ndc);
		assert(NEAR(ndc[0], x, 1.0e-3f));
		assert(NEAR(ndc[1], y, 1.0e-3f));
		// The depth the face stores is the depth the shader compares with
		assert(NEAR(ndc[2] * 0.5f + 0.5f, lamp_shadow_depth(dist, NEAR_PLANE, FAR_PLANE), 1.0e-4f));
	}
}

static void test_a_face_covers_its_ninety_degrees_with_room_to_spare(void) {
	float v[3] = { 100.0f, 99.0f, 99.0f };
	float x, y, dist;

	lamp_shadow_locate(v, &x, &y, &dist);
	// On the diagonal edge of a face: inside clip space, with the margin left over
	assert(fabsf(x) < 1.0f && fabsf(y) < 1.0f);
	v[1] = 100.0f;
	v[2] = 100.0f;
	lamp_shadow_locate(v, &x, &y, &dist);
	assert(fabsf(x) <= 1.0f / (1.0f + LAMP_SHADOW_MARGIN) + 1.0e-4f);
}

static void test_depth_grows_with_distance_and_spans_zero_to_one(void) {
	assert(NEAR(lamp_shadow_depth(NEAR_PLANE, NEAR_PLANE, FAR_PLANE), 0.0f, 1.0e-4f));
	assert(NEAR(lamp_shadow_depth(FAR_PLANE, NEAR_PLANE, FAR_PLANE), 1.0f, 1.0e-4f));
	assert(lamp_shadow_depth(50.0f, NEAR_PLANE, FAR_PLANE) < lamp_shadow_depth(60.0f, NEAR_PLANE, FAR_PLANE));
}

int main(void) {
	test_faces_are_orthonormal();
	test_the_six_faces_face_six_ways();
	test_the_lookup_agrees_with_the_views();
	test_a_face_covers_its_ninety_degrees_with_room_to_spare();
	test_depth_grows_with_distance_and_spans_zero_to_one();
	printf("lamp_shadow_math_test: all passed\n");
	return 0;
}
