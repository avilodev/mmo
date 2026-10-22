// Check the footing map: cells, edges, ramps, levels, fallbacks, bad files

#define _POSIX_C_SOURCE 200809L
#include "footing_fixture.h"
#include "world_spans_fixture.h"
#include "world/footing.h"
#include "world_spans.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>

#define PATH     "/tmp/footing_test.bin"
#define BAD_PATH "/tmp/footing_test_bad.bin"

static int close_to(float a, float b) {
	return fabsf(a - b) < 0.05f;
}

static void test_without_a_map(void) {
	float z = -1.0f;

	world_spans_init(CITY_ENNARA_TILE_X + 8, CITY_ENNARA_TILE_Y + 8, WORLD_TILE_PX);
	assert(!footing_loaded());
	assert(footing_height_at(FF_X(0, 3), FF_Y, 0.0f, &z) && z == 0.0f);
	assert(!footing_load("/tmp/footing_test_does_not_exist.bin"));
	assert(!footing_loaded());
	assert(footing_height_at(FF_X(0, 3), FF_Y, 0.0f, &z) && z == 0.0f);
}

static void test_flat_tiles_use_the_span_floor(void) {
	float z = -1.0f;

	ff_write(PATH, FF_VERSION);
	assert(footing_load(PATH));
	assert(footing_loaded());
	assert(footing_height_at(FF_X(0, 3), FF_Y, 0.0f, &z) && z == 0.0f);
	assert(footing_height_at(FF_X(0, 3), FF_Y, 5.0f, &z) && z == 0.0f);
	// a floor more than a step above the hint is not the floor
	z = -1.0f;
	assert(!footing_height_at(FF_X(0, 3), FF_Y, -20.0f, &z) && z == -1.0f);
	// outside the map, still the span floor
	assert(footing_height_at(FF_X(5, 3), FF_Y, 0.0f, &z) && z == 0.0f);
}

static void test_stairs(void) {
	float z = -1.0f;

	assert(footing_height_at(FF_X(1, 1), FF_Y, 0.0f, &z) && close_to(z, 0.0f));
	assert(footing_height_at(FF_X(1, 5), FF_Y, 0.0f, &z) && close_to(z, 4.0f));
	assert(footing_height_at(FF_X(1, 9), FF_Y, 0.0f, &z) && close_to(z, 8.0f));
	assert(footing_height_at(FF_X(1, 13), FF_Y, 0.0f, &z) && close_to(z, 12.0f));
	// the edge between two treads is a step, not a slope: no blending
	assert(footing_height_at(FF_X(1, 3) + 0.5f, FF_Y, 4.0f, &z) && close_to(z, 4.0f));
	assert(footing_height_at(FF_X(1, 3) + 0.4f, FF_Y, 0.0f, &z) && close_to(z, 0.0f));
	// a tread more than a step above the hint is skipped for the span floor
	assert(footing_height_at(FF_X(1, 13), FF_Y, -1.0f, &z) && close_to(z, 0.0f));
	z = -1.0f;
	assert(!footing_height_at(FF_X(1, 13), FF_Y, -13.0f, &z) && z == -1.0f);
}

static void test_a_tread_beside_a_riser_is_flat(void) {
	float along = -1.0f;
	float across = -1.0f;

	// the next tread, 4 higher, is a step and not a slope
	footing_slope_at(FF_X(1, 3), FF_Y, 0.0f, 1.0f, 0.0f, 2.0f, &along, &across);
	assert(close_to(along, 0.0f) && close_to(across, 0.0f));
}

static void test_ramp(void) {
	float z = -1.0f;
	float along = -1.0f;
	float across = -1.0f;

	assert(footing_height_at(FF_X(2, 8), FF_Y, 4.0f, &z) && close_to(z, 4.0f));
	// between two cells a ramp is smooth
	assert(footing_height_at(FF_X(2, 7) + 0.5f, FF_Y, 4.0f, &z) && close_to(z, 3.75f));
	footing_slope_at(FF_X(2, 8), FF_Y, 4.0f, 1.0f, 0.0f, 2.0f, &along, &across);
	assert(close_to(along, 0.5f) && close_to(across, 0.0f));
	footing_slope_at(FF_X(2, 8), FF_Y, 4.0f, 0.0f, 1.0f, 2.0f, &along, &across);
	assert(close_to(along, 0.0f) && close_to(across, -0.5f));
	footing_slope_at(FF_X(1, 5), FF_Y, 4.0f, 1.0f, 0.0f, 1.0f, &along, &across);
	assert(close_to(along, 0.0f) && close_to(across, 0.0f));
	footing_slope_at(FF_X(0, 3), FF_Y, -20.0f, 1.0f, 0.0f, 1.0f, &along, &across);
	assert(along == 0.0f && across == 0.0f);
}

static void test_a_bridge_over_ground(void) {
	float z = -1.0f;

	assert(footing_height_at(FF_X(3, 5), FF_Y, 0.0f, &z) && z == 0.0f);
	assert(footing_height_at(FF_X(3, 5), FF_Y, 58.0f, &z) && z == 60.0f);
	assert(footing_height_at(FF_X(3, 5), FF_Y, 50.0f, &z) && z == 60.0f);
	// 60 is more than a step above 30, so the ground it is
	assert(footing_height_at(FF_X(3, 5), FF_Y, 30.0f, &z) && z == 0.0f);
}

static void test_bad_input(void) {
	float z = -1.0f;
	float along = 7.0f;
	float across = 7.0f;

	assert(!footing_height_at(NAN, FF_Y, 0.0f, &z));
	assert(!footing_height_at(FF_X(1, 5), INFINITY, 0.0f, &z));
	assert(!footing_height_at(FF_X(1, 5), FF_Y, NAN, &z));
	assert(!footing_height_at(1.0e30f, FF_Y, 0.0f, &z));
	footing_slope_at(NAN, FF_Y, 0.0f, 1.0f, 0.0f, 1.0f, &along, &across);
	assert(along == 0.0f && across == 0.0f);
	footing_slope_at(FF_X(2, 8), FF_Y, 4.0f, 1.0f, 0.0f, 0.0f, &along, &across);
	assert(along == 0.0f && across == 0.0f);
	assert(z == -1.0f);
}

static void test_a_crafted_header_is_refused(void) {
	FILE* f;

	// a map placed so far off that tile arithmetic would overflow
	ff_write_at(PATH, FF_VERSION, 2147483000);
	assert(!footing_load(PATH));
	assert(!footing_loaded());
	ff_write_at(PATH, FF_VERSION, -2147483000);
	assert(!footing_load(PATH));

	// a header that claims a huge grid in a file that holds none of it
	f = fopen(BAD_PATH, "wb");
	fwrite("CFTG", 1, 4, f);
	ff_put_u32(f, FF_VERSION);
	ff_put_u32(f, 0);
	ff_put_u32(f, 0);
	ff_put_u32(f, 20000);
	ff_put_u32(f, 20000);
	ff_put_u32(f, FF_CELLS);
	ff_put_u32(f, 0);
	fputc(0, f);
	fclose(f);
	assert(!footing_load(BAD_PATH));
	assert(!footing_loaded());
}

// Two span floors in the flat fixture tile: 0 under a low ceiling, 40 above it
static void test_the_span_fallback_skips_floors_too_high(void) {
	static const world_span spans[] = { { 0.0f, 40.0f }, { 40.0f, FIX_OPEN } };
	int w = CITY_ENNARA_TILE_X + 8;
	int h = CITY_ENNARA_TILE_Y + 8;
	fix_record rec = { (uint32_t)(CITY_ENNARA_TILE_Y * w + CITY_ENNARA_TILE_X), 0, 2 };
	float z = -1.0f;

	footing_free();
	world_spans_init(w, h, WORLD_TILE_PX);
	fix_write(PATH, w, h, WORLD_TILE_PX, &rec, 1, spans, 2, WORLD_SPANS_VERSION, 0);
	assert(world_spans_load(PATH));

	// 40 is more than a step above 25, so the floor under the feet is 0
	assert(footing_height_at(FF_X(0, 3), FF_Y, 25.0f, &z) && z == 0.0f);
	assert(footing_height_at(FF_X(0, 3), FF_Y, 38.0f, &z) && z == 40.0f);
	assert(footing_height_at(FF_X(0, 3), FF_Y, 52.0f, &z) && z == 40.0f);

	world_spans_init(w, h, WORLD_TILE_PX);
}

static void test_bad_files(void) {
	float z = -1.0f;
	FILE* f;

	ff_write(PATH, FF_VERSION + 1u);
	assert(!footing_load(PATH));
	assert(!footing_loaded());

	ff_write(PATH, FF_VERSION);
	ff_prefix(PATH, BAD_PATH, 200);
	assert(!footing_load(BAD_PATH));
	assert(!footing_loaded());

	f = fopen(BAD_PATH, "wb");
	fwrite("XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX", 1, 34, f);
	fclose(f);
	assert(!footing_load(BAD_PATH));
	assert(!footing_loaded());

	// after a failed load the lookups are the span floors again
	assert(footing_height_at(FF_X(1, 13), FF_Y, 0.0f, &z) && z == 0.0f);

	// a good file loads again, and frees cleanly
	ff_write(PATH, FF_VERSION);
	assert(footing_load(PATH));
	footing_free();
	assert(!footing_loaded());
}

int main(void) {
	test_without_a_map();
	test_flat_tiles_use_the_span_floor();
	test_stairs();
	test_a_tread_beside_a_riser_is_flat();
	test_ramp();
	test_a_bridge_over_ground();
	test_bad_input();
	test_a_crafted_header_is_refused();
	test_the_span_fallback_skips_floors_too_high();
	test_bad_files();
	footing_free();
	printf("footing_test: OK\n");

	return 0;
}
