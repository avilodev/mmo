// Check the field of roof heights: lookups, edges, and what counts as covered.
#include "world/cover_field.h"
#include <assert.h>
#include <stdio.h>

#define NEAR(a, b) ((a) > (b) - 0.001f && (a) < (b) + 0.001f)

// A 4 by 3 grid of 10-unit cells from (100, 200): a roof over the middle two
static const float GRID[12] = {
	-100.0f, -100.0f, -100.0f, -100.0f,
	-100.0f, 80.0f, 80.0f, -100.0f,
	-100.0f, -100.0f, 5.0f, -100.0f
};

static void test_nothing_is_known_before_it_is_set(void) {
	float h;

	cover_field_clear();
	assert(!cover_field_height(100.0f, 200.0f, &h));
	assert(!cover_field_covered(100.0f, 200.0f, 0.0f, 20.0f));
}

static void test_a_height_is_read_from_its_cell(void) {
	float h = 0.0f;

	cover_field_set(GRID, 4, 3, 100.0f, 200.0f, 10.0f, 10.0f);
	assert(cover_field_height(115.0f, 215.0f, &h) && NEAR(h, 80.0f));
	assert(cover_field_height(125.0f, 215.0f, &h) && NEAR(h, 80.0f));
	assert(cover_field_height(125.0f, 225.0f, &h) && NEAR(h, 5.0f));
	assert(cover_field_height(100.0f, 200.0f, &h) && NEAR(h, -100.0f));
}

static void test_outside_the_grid_is_unknown(void) {
	float h;

	cover_field_set(GRID, 4, 3, 100.0f, 200.0f, 10.0f, 10.0f);
	assert(!cover_field_height(99.9f, 210.0f, &h));
	assert(!cover_field_height(140.1f, 210.0f, &h));
	assert(!cover_field_height(110.0f, 199.9f, &h));
	assert(!cover_field_height(110.0f, 230.1f, &h));
	assert(cover_field_height(139.9f, 229.9f, &h));
}

static void test_under_a_roof_is_covered_and_on_it_is_not(void) {
	cover_field_set(GRID, 4, 3, 100.0f, 200.0f, 10.0f, 10.0f);
	assert(cover_field_covered(115.0f, 215.0f, 0.0f, 20.0f));
	assert(cover_field_covered(115.0f, 215.0f, 50.0f, 20.0f));
	assert(!cover_field_covered(115.0f, 215.0f, 70.0f, 20.0f));
	assert(!cover_field_covered(115.0f, 215.0f, 80.0f, 20.0f));
}

static void test_open_ground_and_low_things_are_not_cover(void) {
	cover_field_set(GRID, 4, 3, 100.0f, 200.0f, 10.0f, 10.0f);
	assert(!cover_field_covered(105.0f, 205.0f, 0.0f, 20.0f));
	// A bench 5 units high is no roof for someone standing at the ground
	assert(!cover_field_covered(125.0f, 225.0f, 0.0f, 20.0f));
	// Off the grid is open sky
	assert(!cover_field_covered(0.0f, 0.0f, 0.0f, 20.0f));
}

static void test_the_grid_is_copied(void) {
	float local[2] = { 50.0f, 50.0f };
	float h = 0.0f;

	cover_field_set(local, 2, 1, 0.0f, 0.0f, 10.0f, 10.0f);
	local[0] = 0.0f;
	assert(cover_field_height(5.0f, 5.0f, &h) && NEAR(h, 50.0f));
	cover_field_set(local, 0, 0, 0.0f, 0.0f, 10.0f, 10.0f);
	assert(!cover_field_height(5.0f, 5.0f, &h));
}

int main(void) {
	test_nothing_is_known_before_it_is_set();
	test_a_height_is_read_from_its_cell();
	test_outside_the_grid_is_unknown();
	test_under_a_roof_is_covered_and_on_it_is_not();
	test_open_ground_and_low_things_are_not_cover();
	test_the_grid_is_copied();
	printf("cover_field_test: all passed\n");

	return 0;
}
