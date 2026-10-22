// Check the shadow map's matrices: placement, depth order and stability.
#include "world/shadow_math.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>

#define MAP 2048
#define RADIUS 200.0f
#define NEAR(a, b, eps) (fabsf((a) - (b)) < (eps))

static const float DOWN[3] = { 0.0f, 1.0f, 0.0f };

static void unit(float* v) {
	float len = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);

	for(int i = 0; i < 3; i++)
		v[i] /= len;
}

static void test_the_centre_lands_in_the_middle(void) {
	float light[3] = { -0.4f, 0.8f, -0.3f };
	float center[3] = { 1234.5f, 40.0f, -987.6f };
	shadow_cascade c;
	float uvz[3];

	unit(light);
	shadow_cascade_fit(&c, light, center, RADIUS, MAP);
	assert(shadow_cascade_project(&c, center, uvz));
	// Snapping slides the box by under one texel
	assert(NEAR(uvz[0], 0.5f, 1.0f / MAP));
	assert(NEAR(uvz[1], 0.5f, 1.0f / MAP));
	assert(uvz[2] > 0.0f && uvz[2] < 1.0f);
}

static void test_the_box_is_as_wide_as_the_radius(void) {
	shadow_cascade c;
	float center[3] = { 0.0f, 0.0f, 0.0f };
	float in[3] = { 0.9f * RADIUS, 0.0f, 0.0f };
	float out[3] = { 1.1f * RADIUS, 0.0f, 0.0f };
	float uvz[3];

	shadow_cascade_fit(&c, DOWN, center, RADIUS, MAP);
	assert(shadow_cascade_project(&c, in, uvz));
	assert(!shadow_cascade_project(&c, out, uvz));
	assert(NEAR(c.texel, 2.0f * RADIUS / MAP, 1.0e-5f));
}

static void test_depth_grows_away_from_the_light(void) {
	float light[3] = { 0.3f, 0.7f, 0.2f };
	float center[3] = { 50.0f, 10.0f, 80.0f };
	shadow_cascade c;
	float toward[3], away[3], mid[3];

	unit(light);
	shadow_cascade_fit(&c, light, center, RADIUS, MAP);
	for(int i = 0; i < 3; i++) {
		toward[i] = center[i] + light[i] * 60.0f;
		away[i] = center[i] - light[i] * 60.0f;
		mid[i] = center[i];
	}
	float a[3], b[3], m[3];
	assert(shadow_cascade_project(&c, toward, a));
	assert(shadow_cascade_project(&c, away, b));
	assert(shadow_cascade_project(&c, mid, m));
	assert(a[2] < m[2] && m[2] < b[2]);
}

static void test_a_tall_caster_toward_the_light_is_inside_the_depth_range(void) {
	float light[3] = { 0.0f, 1.0f, 0.0f };
	float center[3] = { 0.0f, 0.0f, 0.0f };
	float top[3] = { 0.0f, 500.0f, 0.0f };
	shadow_cascade c;
	float uvz[3];

	shadow_cascade_fit(&c, light, center, RADIUS, MAP);
	assert(shadow_cascade_project(&c, top, uvz));
}

static void test_moving_a_whole_texel_shifts_the_picture_one_texel(void) {
	float light[3] = { -0.4f, 0.8f, -0.3f };
	float a_center[3] = { 100.0f, 0.0f, 100.0f };
	float c_center[3];
	shadow_cascade a, c;
	float p[3] = { 12.0f, 3.0f, -7.0f };
	float uv_a[3], uv_c[3];

	unit(light);
	shadow_cascade_fit(&a, light, a_center, RADIUS, MAP);
	// The map's own x direction, from the first row of its view matrix
	float d[3] = { a.view[0], a.view[4], a.view[8] };

	for(int i = 0; i < 3; i++)
		c_center[i] = a_center[i] + d[i] * a.texel;
	shadow_cascade_fit(&c, light, c_center, RADIUS, MAP);
	shadow_cascade_project(&a, p, uv_a);
	shadow_cascade_project(&c, p, uv_c);
	assert(NEAR(uv_a[0] - uv_c[0], 1.0f / MAP, 1.0e-4f));
	assert(NEAR(uv_a[1], uv_c[1], 1.0e-4f));
}

static void test_sub_texel_moves_never_slide_the_picture_between_snaps(void) {
	shadow_cascade first;
	float p[3] = { 12.0f, 3.0f, -7.0f };
	float base[3] = { 0.0f, 0.0f, 0.0f };

	shadow_cascade_fit(&first, DOWN, base, RADIUS, MAP);
	// Straight down, the map's x axis is world x. Slide the centre in steps
	// of a twentieth of a texel: the picture only ever moves in whole texels.
	float uv0[3];

	shadow_cascade_project(&first, p, uv0);
	for(int step = 0; step < 40; step++) {
		float center[3] = { first.texel * 0.05f * (float)step, 0.0f, 0.0f };
		shadow_cascade c;
		float uv[3];
		float texels;

		shadow_cascade_fit(&c, DOWN, center, RADIUS, MAP);
		shadow_cascade_project(&c, p, uv);
		texels = (uv0[0] - uv[0]) * MAP;
		assert(NEAR(texels, roundf(texels), 1.0e-2f));
	}
}

static void test_every_direction_gives_finite_matrices(void) {
	for(int i = 0; i < 200; i++) {
		float a = (float)i * 0.0314f;
		float light[3] = { cosf(a) * 0.3f, 0.2f + 0.8f * (float)(i % 5) / 4.0f, sinf(a) * 0.3f };
		float center[3] = { 10.0f, 0.0f, 10.0f };
		shadow_cascade c;

		unit(light);
		shadow_cascade_fit(&c, light, center, RADIUS, MAP);
		for(int k = 0; k < 16; k++)
			assert(isfinite(c.viewproj[k]) && isfinite(c.tex[k]));
	}
}

static void test_a_map_never_drawn_is_always_due(void) {
	assert(shadow_cascade_redraw_due(-1, 0.0f, RADIUS, 2));
	assert(shadow_cascade_redraw_due(-1, 0.0f, RADIUS, 1000));
}

static void test_an_interval_of_one_redraws_every_frame(void) {
	assert(shadow_cascade_redraw_due(1, 0.0f, RADIUS, 1));
}

static void test_a_map_is_kept_until_its_interval_is_up(void) {
	assert(!shadow_cascade_redraw_due(0, 0.0f, RADIUS, 2));
	assert(!shadow_cascade_redraw_due(1, 0.0f, RADIUS, 2));
	assert(shadow_cascade_redraw_due(2, 0.0f, RADIUS, 2));
	assert(shadow_cascade_redraw_due(5, 0.0f, RADIUS, 2));
}

static void test_a_map_is_redrawn_early_when_the_centre_has_moved_far(void) {
	assert(!shadow_cascade_redraw_due(1, 0.1f * RADIUS, RADIUS, 4));
	assert(shadow_cascade_redraw_due(1, 0.5f * RADIUS, RADIUS, 4));
}

int main(void) {
	test_a_map_never_drawn_is_always_due();
	test_an_interval_of_one_redraws_every_frame();
	test_a_map_is_kept_until_its_interval_is_up();
	test_a_map_is_redrawn_early_when_the_centre_has_moved_far();
	test_the_centre_lands_in_the_middle();
	test_the_box_is_as_wide_as_the_radius();
	test_depth_grows_away_from_the_light();
	test_a_tall_caster_toward_the_light_is_inside_the_depth_range();
	test_moving_a_whole_texel_shifts_the_picture_one_texel();
	test_sub_texel_moves_never_slide_the_picture_between_snaps();
	test_every_direction_gives_finite_matrices();
	printf("shadow_math_test: all passed\n");
	return 0;
}
