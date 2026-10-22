// Check the sky: its colours and lights follow the hour of the day.
#include "world/sky.h"
#include "world/world_clock.h"
#include "render/world_light.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>

#define NEAR(a, b) (fabsf((a) - (b)) < 1.0e-4f)

static float length3(const float* v) {
	return sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}

static float largest_gap(const sky_state* a, const sky_state* b) {
	float gap = 0.0f;

	for(int i = 0; i < 3; i++) {
		gap = fmaxf(gap, fabsf(a->zenith[i] - b->zenith[i]));
		gap = fmaxf(gap, fabsf(a->horizon[i] - b->horizon[i]));
		gap = fmaxf(gap, fabsf(a->haze[i] - b->haze[i]));
	}

	return gap;
}

static void test_noon_horizon_is_the_old_sky_colour(void) {
	sky_state s;

	sky_at(12.0f, &s);
	assert(NEAR(s.horizon[0], WORLD_SKY_R));
	assert(NEAR(s.horizon[1], WORLD_SKY_G));
	assert(NEAR(s.horizon[2], WORLD_SKY_B));
}

static void test_ground_haze_is_a_little_darker_than_the_sky_above_it(void) {
	// So the horizon reads as a line between land and sky, all day
	for(float h = 0.0f; h < 24.0f; h += 0.25f) {
		sky_state s;

		sky_at(h, &s);
		for(int i = 0; i < 3; i++) {
			assert(s.haze[i] <= s.horizon[i]);
			assert(s.haze[i] >= s.horizon[i] * 0.7f);
		}
	}
}

static void test_sun_is_up_at_noon_and_down_at_midnight(void) {
	sky_state noon, midnight;

	sky_at(12.0f, &noon);
	sky_at(0.0f, &midnight);
	assert(noon.sun_dir[1] > 0.9f);
	assert(midnight.sun_dir[1] < -0.9f);
}

static void test_sun_rises_in_the_east_and_sets_in_the_west(void) {
	sky_state dawn, dusk;

	sky_at(6.0f, &dawn);
	sky_at(18.0f, &dusk);
	assert(dawn.sun_dir[0] > 0.9f);
	assert(NEAR(dawn.sun_dir[1], 0.0f));
	assert(dusk.sun_dir[0] < -0.9f);
	assert(NEAR(dusk.sun_dir[1], 0.0f));
}

static void test_directions_are_unit_and_moon_is_opposite(void) {
	for(float h = 0.0f; h < 24.0f; h += 0.5f) {
		sky_state s;

		sky_at(h, &s);
		assert(NEAR(length3(s.sun_dir), 1.0f));
		assert(NEAR(length3(s.moon_dir), 1.0f));
		for(int i = 0; i < 3; i++)
			assert(NEAR(s.moon_dir[i], -s.sun_dir[i]));
	}
}

static void test_stars_come_out_at_night_only(void) {
	sky_state noon, midnight, dusk;

	sky_at(12.0f, &noon);
	sky_at(0.0f, &midnight);
	sky_at(18.0f, &dusk);
	assert(noon.star_amount == 0.0f);
	assert(midnight.star_amount == 1.0f);
	assert(dusk.star_amount > 0.0f && dusk.star_amount < 1.0f);
}

static void test_sun_colour_warms_toward_the_horizon(void) {
	sky_state noon, dusk;

	sky_at(12.0f, &noon);
	sky_at(18.0f, &dusk);
	// Redder at the horizon: less blue relative to red
	assert(dusk.sun_color[2] / dusk.sun_color[0] < noon.sun_color[2] / noon.sun_color[0]);
}

static void test_colours_stay_in_range(void) {
	for(float h = 0.0f; h < 24.0f; h += 0.05f) {
		sky_state s;

		sky_at(h, &s);
		for(int i = 0; i < 3; i++) {
			assert(s.zenith[i] >= 0.0f && s.zenith[i] <= 1.0f);
			assert(s.horizon[i] >= 0.0f && s.horizon[i] <= 1.0f);
			assert(s.haze[i] >= 0.0f && s.haze[i] <= 1.0f);
			assert(s.sun_color[i] >= 0.0f && s.sun_color[i] <= 1.0f);
		}
		assert(s.star_amount >= 0.0f && s.star_amount <= 1.0f);
	}
}

static void test_colours_change_smoothly_all_day_and_across_midnight(void) {
	sky_state prev;

	sky_at(0.0f, &prev);
	for(int i = 1; i <= 2400; i++) {
		sky_state s;

		sky_at((float)i * 0.01f, &s);
		assert(largest_gap(&prev, &s) < 0.02f);
		prev = s;
	}
}

static void test_hours_outside_a_day_wrap(void) {
	sky_state a, b;

	sky_at(-1.0f, &a);
	sky_at(23.0f, &b);
	assert(largest_gap(&a, &b) < 1.0e-4f);
	sky_at(25.0f, &a);
	sky_at(1.0f, &b);
	assert(largest_gap(&a, &b) < 1.0e-4f);
	sky_at(24.0f, &a);
	sky_at(0.0f, &b);
	assert(largest_gap(&a, &b) < 1.0e-4f);
}

static void test_current_sky_is_noon_until_updated(void) {
	sky_state noon;

	sky_at(12.0f, &noon);
	assert(largest_gap(sky_current(), &noon) == 0.0f);
	sky_update(0.0f);
	assert(largest_gap(sky_current(), &noon) > 0.1f);
}

static void test_clock_gives_the_hour_it_was_ticked_at(void) {
	world_clock_reset();
	world_clock_sync(1350000u, 2700000u, 0.0);
	world_clock_tick(0.0);
	assert(NEAR(world_clock_hour_now(), 12.0f));
	world_clock_tick(112.5);
	assert(NEAR(world_clock_hour_now(), 13.0f));
}

int main(void) {
	test_noon_horizon_is_the_old_sky_colour();
	test_ground_haze_is_a_little_darker_than_the_sky_above_it();
	test_sun_is_up_at_noon_and_down_at_midnight();
	test_sun_rises_in_the_east_and_sets_in_the_west();
	test_directions_are_unit_and_moon_is_opposite();
	test_stars_come_out_at_night_only();
	test_sun_colour_warms_toward_the_horizon();
	test_colours_stay_in_range();
	test_colours_change_smoothly_all_day_and_across_midnight();
	test_hours_outside_a_day_wrap();
	test_current_sky_is_noon_until_updated();
	test_clock_gives_the_hour_it_was_ticked_at();
	printf("sky_test: all passed\n");
	return 0;
}
