// Check the world's lighting: it follows the hour and keeps noon as it was.
#include "world/world_lighting.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>

#define NEAR(a, b, eps) (fabsf((a) - (b)) < (eps))

static const float UP[3] = { 0.0f, 1.0f, 0.0f };
static const float SIDE[3] = { 1.0f, 0.0f, 0.0f };

static void lighting_at(float hour, world_lighting* out) {
	sky_state sky;

	sky_at(hour, &sky);
	world_lighting_from_sky(&sky, out);
}

static float brightness(const world_lighting* l, const float* normal) {
	float c[3];

	world_lighting_surface(l, normal, c);
	return (c[0] + c[1] + c[2]) / 3.0f;
}

static void test_noon_floor_is_exactly_bright(void) {
	world_lighting l;
	float c[3];

	lighting_at(12.0f, &l);
	world_lighting_surface(&l, UP, c);
	for(int i = 0; i < 3; i++)
		assert(NEAR(c[i], 1.0f, 0.02f));
}

static void test_noon_shaded_side_matches_the_old_look(void) {
	world_lighting l;
	float b;

	lighting_at(12.0f, &l);
	b = brightness(&l, SIDE);
	// The old fixed sun left a side facing away at about 0.47
	assert(b > 0.30f && b < 0.65f);
}

static void test_noon_has_full_shadows_and_no_lamps(void) {
	world_lighting l;

	lighting_at(12.0f, &l);
	assert(l.shadow_strength == 1.0f);
	assert(l.lamp_amount == 0.0f);
}

static void test_night_is_dim_but_readable_and_lamps_are_on(void) {
	world_lighting l;
	float b;

	lighting_at(0.0f, &l);
	b = brightness(&l, UP);
	assert(b > 0.15f && b < 0.45f);
	assert(l.lamp_amount == 1.0f);
	assert(l.shadow_strength > 0.0f && l.shadow_strength <= 0.5f);
	// The light is the moon: it shines from above the horizon
	assert(l.dir[1] > 0.5f);
}

static void test_light_direction_is_always_a_unit_vector(void) {
	for(float h = 0.0f; h < 24.0f; h += 0.01f) {
		world_lighting l;
		float len;

		lighting_at(h, &l);
		len = sqrtf(l.dir[0] * l.dir[0] + l.dir[1] * l.dir[1] + l.dir[2] * l.dir[2]);
		assert(NEAR(len, 1.0f, 1.0e-3f));
	}
}

static void test_brightness_changes_smoothly_all_day(void) {
	world_lighting prev;
	float prev_b;

	lighting_at(0.0f, &prev);
	prev_b = brightness(&prev, UP);
	for(int i = 1; i <= 2400; i++) {
		world_lighting l;
		float b;

		lighting_at((float)i * 0.01f, &l);
		b = brightness(&l, UP);
		assert(NEAR(b, prev_b, 0.03f));
		assert(NEAR(l.shadow_strength, prev.shadow_strength, 0.08f));
		assert(NEAR(l.lamp_amount, prev.lamp_amount, 0.05f));
		prev = l;
		prev_b = b;
	}
}

static void test_twilight_is_not_darker_than_the_night_after_it(void) {
	world_lighting dusk, night;

	lighting_at(18.5f, &dusk);
	lighting_at(23.0f, &night);
	assert(brightness(&dusk, UP) >= brightness(&night, UP) - 0.01f);
}

static void test_dawn_and_dusk_light_is_warm(void) {
	world_lighting l;

	lighting_at(17.5f, &l);
	assert(l.color[0] > l.color[2]);
}

static void test_noon_helper_has_no_shadows_or_lamps(void) {
	world_lighting l;

	world_lighting_noon(&l);
	assert(l.shadow_strength == 0.0f);
	assert(l.lamp_amount == 0.0f);
	assert(NEAR(brightness(&l, UP), 1.0f, 0.02f));
}

static void test_current_is_noon_until_updated(void) {
	sky_state sky;

	assert(NEAR(brightness(world_lighting_current(), UP), 1.0f, 0.02f));
	sky_at(0.0f, &sky);
	world_lighting_update(&sky);
	assert(brightness(world_lighting_current(), UP) < 0.5f);
}

int main(void) {
	test_noon_floor_is_exactly_bright();
	test_noon_shaded_side_matches_the_old_look();
	test_noon_has_full_shadows_and_no_lamps();
	test_night_is_dim_but_readable_and_lamps_are_on();
	test_light_direction_is_always_a_unit_vector();
	test_brightness_changes_smoothly_all_day();
	test_twilight_is_not_darker_than_the_night_after_it();
	test_dawn_and_dusk_light_is_warm();
	test_noon_helper_has_no_shadows_or_lamps();
	test_current_is_noon_until_updated();
	printf("world_lighting_test: all passed\n");
	return 0;
}
