// Check which font bake is chosen for a size on screen.

#include "render/font_levels.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>

#define BASE 22.0f

static int near(float a, float b) {
	return fabsf(a - b) < 0.01f;
}

static void test_each_level_is_root_two_bigger(void) {
	assert(near(font_level_size(0, BASE), 22.0f));
	assert(near(font_level_size(2, BASE), 44.0f));
	assert(near(font_level_size(4, BASE), 88.0f));
	for(int level = 1; level < FONT_LEVEL_COUNT; level++) {
		float ratio = font_level_size(level, BASE) / font_level_size(level - 1, BASE);

		assert(near(ratio, 1.41421356f));
	}
}

static void test_a_level_out_of_range_is_held_to_the_ends(void) {
	assert(near(font_level_size(-3, BASE), 22.0f));
	assert(near(font_level_size(99, BASE), 88.0f));
}

static void test_a_baked_size_picks_its_own_bake(void) {
	for(int level = 0; level < FONT_LEVEL_COUNT; level++)
		assert(font_level_for(font_level_size(level, BASE), BASE) == level);
}

static void test_in_between_sizes_pick_the_nearer_bake(void) {
	assert(font_level_for(24.0f, BASE) == 0);
	assert(font_level_for(27.0f, BASE) == 1);
	assert(font_level_for(36.0f, BASE) == 1);
	assert(font_level_for(40.0f, BASE) == 2);
	assert(font_level_for(52.0f, BASE) == 2);
	assert(font_level_for(55.0f, BASE) == 3);
	assert(font_level_for(75.0f, BASE) == 4);
}

static void test_sizes_beyond_the_ends_use_the_end_bakes(void) {
	assert(font_level_for(1.0f, BASE) == 0);
	assert(font_level_for(0.0f, BASE) == 0);
	assert(font_level_for(-5.0f, BASE) == 0);
	assert(font_level_for(400.0f, BASE) == FONT_LEVEL_COUNT - 1);
}

static void test_a_bake_is_never_stretched_much(void) {
	// Between the smallest and biggest bake, the one chosen is within 1.2 of it
	for(float px = 22.0f; px <= 88.0f; px += 0.5f) {
		float bake = font_level_size(font_level_for(px, BASE), BASE);
		float stretch = px > bake ? px / bake : bake / px;

		assert(stretch < 1.2f);
	}
}

static void test_a_zero_base_does_not_divide_by_zero(void) {
	assert(font_level_for(30.0f, 0.0f) == 0);
}

int main(void) {
	test_each_level_is_root_two_bigger();
	test_a_level_out_of_range_is_held_to_the_ends();
	test_a_baked_size_picks_its_own_bake();
	test_in_between_sizes_pick_the_nearer_bake();
	test_sizes_beyond_the_ends_use_the_end_bakes();
	test_a_bake_is_never_stretched_much();
	test_a_zero_base_does_not_divide_by_zero();
	printf("font_levels_test: all passed\n");
	return 0;
}
