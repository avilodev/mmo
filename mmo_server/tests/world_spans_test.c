// Check the span lookups: flat default, ramp, step, bridge, headroom, loader.

#define _POSIX_C_SOURCE 200809L
#include "world_spans_fixture.h"
#include "world_mask_fixture.h"

#include <assert.h>
#include <sys/stat.h>
#include <math.h>
#include <stdio.h>

#define DIR "/tmp/world_spans_test_dir/"
#define PATH DIR "world_spans.bin"
#define MASK DIR "mask.bin"
#define MASK_BAD DIR "mask_bad.bin"
#define Y5 FIX_CX(5)

static void test_flat_default(void) {
	float z = -1.0f;

	world_spans_init(FIX_W, FIX_H, FIX_TILE);
	assert(!world_spans_loaded());
	assert(world_spans_reach(FIX_CX(2), FIX_CX(2), 0.0f, &z) && z == 0.0f);
	assert(world_spans_reach(FIX_CX(2), FIX_CX(2), 11.9f, &z) && z == 0.0f);
	assert(!world_spans_reach(FIX_CX(2), FIX_CX(2), 12.1f, &z));
	assert(!world_spans_reach(FIX_CX(2), FIX_CX(2), 100.0f, &z));
	assert(!world_spans_reach(-1.0f, 8.0f, 0.0f, &z));
	assert(!world_spans_reach(1024.0f, 8.0f, 0.0f, &z));
	assert(!world_spans_reach(NAN, 8.0f, 0.0f, &z));
	assert(!world_spans_reach(8.0f, 8.0f, NAN, &z));
}

static void test_ramp(void) {
	float z = -1.0f;

	assert(world_spans_walk(FIX_CX(10), Y5, 0.0f, FIX_CX(13), Y5, 6.0f, &z));
	assert(z == 24.0f);
	assert(world_spans_walk(FIX_CX(13), Y5, 24.0f, FIX_CX(10), Y5, 6.0f, &z));
	assert(z == 0.0f);

	// 24 -> 60 is a 36 unit step: not walkable
	assert(!world_spans_walk(FIX_CX(13), Y5, 24.0f, FIX_CX(14), Y5, 6.0f, &z));
}

static void test_walk_huge_distance(void) {
	float z = -1.0f;

	// a normal 3 tile walk still works
	assert(world_spans_walk(FIX_CX(10), Y5, 0.0f, FIX_CX(13), Y5, 6.0f, &z));
	assert(!world_spans_walk(8.0f, 8.0f, 0.0f, 1.0e30f, 8.0f, 6.0f, &z));
	assert(!world_spans_walk(8.0f, 8.0f, 0.0f, 5.0e10f, 8.0f, 6.0f, &z));
	assert(!world_spans_walk(8.0f, 8.0f, 0.0f, INFINITY, 8.0f, 6.0f, &z));
}

static void test_bridge(void) {
	world_span s[WORLD_SPANS_MAX_PER_TILE];
	float z = -1.0f;

	assert(world_spans_at(FIX_CX(20), Y5, s, WORLD_SPANS_MAX_PER_TILE) == 2);
	assert(world_spans_reach(FIX_CX(20), Y5, 0.0f, &z) && z == 0.0f);
	assert(world_spans_reach(FIX_CX(20), Y5, 74.0f, &z) && z == 74.0f);
	assert(!world_spans_reach(FIX_CX(20), Y5, 40.0f, &z));
	assert(world_spans_snap(FIX_CX(20), Y5, 80.0f, &z) && z == 74.0f);
	assert(world_spans_snap(FIX_CX(20), Y5, 5.0f, &z) && z == 0.0f);

	// walking under the deck stays on the ground
	assert(world_spans_walk(FIX_CX(19), Y5, 0.0f, FIX_CX(21), Y5, 6.0f, &z));
	assert(z == 0.0f);
}

static void test_headroom_and_no_floor(void) {
	float z = -1.0f;

	// ceiling 30 over floor 0: less than a body
	assert(!world_spans_reach(FIX_CX(22), Y5, 0.0f, &z));
	assert(!world_spans_snap(FIX_CX(22), Y5, 0.0f, &z));

	// a record with no spans: nothing to stand on
	assert(!world_spans_reach(FIX_CX(24), Y5, 0.0f, &z));
	assert(!world_spans_snap(FIX_CX(24), Y5, 0.0f, &z));
}

// A saved z that matches nothing is replaced by the nearest floor.
static void test_snap_for_login(void) {
	float z = -1.0f;

	// bridge column: 30 sits between the surfaces but nearer the ground (0),
	// and 60 is nearer the deck (74)
	assert(world_spans_snap(FIX_CX(20), Y5, 30.0f, &z) && z == 0.0f);
	assert(world_spans_snap(FIX_CX(20), Y5, 60.0f, &z) && z == 74.0f);
	// a column with nothing to stand on has no answer; the caller keeps its z
	assert(!world_spans_snap(FIX_CX(24), Y5, 40.0f, &z));
}

static void test_free_and_landing(void) {
	float z = -1.0f;

	// in the air above flat ground is free; inside a taller tile is not
	assert(world_spans_free_box(FIX_CX(2), FIX_CX(2), 6.0f, 20.0f));
	assert(world_spans_fits_box(FIX_CX(2), FIX_CX(2), 6.0f, 0.0f));
	assert(!world_spans_fits_box(FIX_CX(2), FIX_CX(2), 6.0f, 20.0f));
	assert(!world_spans_free_box(FIX_CX(14), Y5, 6.0f, 24.0f));
	assert(world_spans_free_box(FIX_CX(14), Y5, 6.0f, 50.0f));

	// the deck leaves 70 over the ground, so a body at 31 would hit it
	assert(world_spans_free_box(FIX_CX(20), Y5, 6.0f, 30.0f));
	assert(!world_spans_free_box(FIX_CX(20), Y5, 6.0f, 31.0f));

	// falling from 110 toward 90 across the platform at 100 lands on it
	assert(world_spans_floor_between(FIX_CX(29), Y5, 6.0f, 90.0f, 122.0f, &z));
	assert(z == 100.0f);
	assert(!world_spans_floor_between(FIX_CX(29), Y5, 6.0f, 101.0f, 122.0f, &z));

	// a point inside the gap under the deck, and one inside the deck's slab
	assert(world_spans_free_point(FIX_CX(20), Y5, 40.0f));
	assert(!world_spans_free_point(FIX_CX(20), Y5, 72.0f));
	assert(world_spans_free_point(FIX_CX(20), Y5, 80.0f));
	assert(!world_spans_free_point(FIX_CX(24), Y5, 10.0f));
}

static void expect_load_fails(const char* what) {
	float z = -1.0f;

	assert(!world_spans_load(PATH));

	// the previous tables survive a refused load
	assert(world_spans_loaded());
	assert(world_spans_reach(FIX_CX(11), Y5, 8.0f, &z) && z == 8.0f);
	(void)what;
}

static void test_loader_rejects_bad_files(void) {
	static const fix_record one[] = {{FIX_T(1, 1), 0, 1}};
	static const fix_record reversed[] = {{FIX_T(2, 1), 0, 1}, {FIX_T(1, 1), 1, 1}};
	static const fix_record two[] = {{FIX_T(1, 1), 0, 2}};
	static const world_span flat[] = {{0, FIX_OPEN}};
	static const world_span flat2[] = {{0, FIX_OPEN}, {0, FIX_OPEN}};
	static const world_span overlap[] = {{0, 50}, {40, FIX_OPEN}};
	static const world_span nan_span[] = {{NAN, FIX_OPEN}};
	static const world_span inverted[] = {{50, 10}};

	fix_write_city(PATH);
	assert(world_spans_load(PATH) && world_spans_loaded());

	fix_write(PATH, FIX_W, FIX_H, FIX_TILE, one, 1, flat, 1, 2, 0);
	expect_load_fails("version");
	fix_write(PATH, FIX_W + 1, FIX_H, FIX_TILE, one, 1, flat, 1, 1, 0);
	expect_load_fails("width");
	fix_write(PATH, FIX_W, FIX_H, FIX_TILE + 1, one, 1, flat, 1, 1, 0);
	expect_load_fails("tile size");
	fix_write(PATH, FIX_W, FIX_H, FIX_TILE, one, 1, flat, 1, 1, 1);
	expect_load_fails("truncated");
	fix_write(PATH, FIX_W, FIX_H, FIX_TILE, reversed, 2, flat2, 2, 1, 0);
	expect_load_fails("unsorted");
	fix_write(PATH, FIX_W, FIX_H, FIX_TILE, two, 1, overlap, 2, 1, 0);
	expect_load_fails("overlap");
	fix_write(PATH, FIX_W, FIX_H, FIX_TILE, one, 1, nan_span, 1, 1, 0);
	expect_load_fails("nan");
	fix_write(PATH, FIX_W, FIX_H, FIX_TILE, one, 1, inverted, 1, 1, 0);
	expect_load_fails("inverted");
	assert(!world_spans_load("/tmp/does_not_exist_spans.bin"));
}

// A box with only some corners over a floor is still supported by that floor
static void test_partial_support(void) {
	float z = -1.0f;

	// on the ramp, corners over floors 0 and 8: the higher is the support
	assert(world_spans_support(FIX_CX(10) + 8.0f, Y5, 6.0f, -12.0f, 12.0f, &z) && z == 8.0f);
	// the platform at 100 beside open ground: right corners hang over the drop
	assert(world_spans_support(FIX_CX(29) + 7.0f, Y5, 6.0f, 88.0f, 112.0f, &z) && z == 100.0f);
	// fully past the edge no corner has a floor near 100
	z = -1.0f;
	assert(!world_spans_support(FIX_CX(30), Y5, 6.0f, 88.0f, 112.0f, &z) && z == -1.0f);
	// a floor the box cannot stand at without overlapping solid is no support
	assert(!world_spans_support(FIX_CX(22), Y5, 6.0f, 60.0f, 80.0f, &z));
	assert(!world_spans_support(NAN, Y5, 6.0f, 0.0f, 12.0f, &z));
	assert(!world_spans_support(FIX_CX(10), Y5, 6.0f, NAN, 12.0f, &z));
}

// The fine mask drops a span at the points whose cell it marks empty
static void test_mask(void) {
	static const mask_rec half_ramp[] = { { .tx = 12, .ty = 5, .levels = 1, .cols = { 0x00FF }, .floor = { 16.0f } } };
	static const mask_rec wrong_count[] = { { .tx = 20, .ty = 5, .levels = 1, .cols = { 0x0000 }, .floor = { 0.0f } } };
	world_span s[WORLD_SPANS_MAX_PER_TILE];
	float z = -1.0f;
	float tile12 = 12.0f * FIX_TILE;

	world_spans_set_mask_origin(0, 0);
	assert(!world_spans_mask_loaded());
	assert(world_spans_at(tile12 + 12.0f, Y5, s, WORLD_SPANS_MAX_PER_TILE) == 1);

	// tile 12 (the ramp at 16) has floor in its columns 0-7 only
	mask_write(MASK, 4, 12, 5, 1, 1, half_ramp, 1);
	assert(world_spans_load_mask(MASK));
	assert(world_spans_mask_loaded());
	assert(world_spans_at(tile12 + 7.99f, Y5, s, WORLD_SPANS_MAX_PER_TILE) == 1);
	assert(s[0].floor == 16.0f);
	assert(world_spans_at(tile12 + 8.0f, Y5, s, WORLD_SPANS_MAX_PER_TILE) == 0);
	assert(world_spans_at(tile12 + 12.0f, Y5, s, WORLD_SPANS_MAX_PER_TILE) == 0);
	// a tile with no record is whole
	assert(world_spans_at(FIX_CX(11), Y5, s, WORLD_SPANS_MAX_PER_TILE) == 1);

	// the queries that look at a point see the edge
	assert(!world_spans_reach(tile12 + 9.0f, Y5, 16.0f, &z));
	assert(world_spans_reach(tile12 + 3.0f, Y5, 16.0f, &z) && z == 16.0f);
	assert(world_spans_fits_box(tile12 + 1.0f, Y5, 6.0f, 16.0f));
	assert(!world_spans_fits_box(tile12 + 4.0f, Y5, 6.0f, 16.0f));
	assert(!world_spans_free_box(tile12 + 4.0f, Y5, 6.0f, 16.0f));

	// a mask with the wrong number of levels for the tile is ignored
	mask_write(MASK, 4, 20, 5, 1, 1, wrong_count, 1);
	assert(world_spans_load_mask(MASK));
	assert(world_spans_at(FIX_CX(20), Y5, s, WORLD_SPANS_MAX_PER_TILE) == 2);

	// shutting the spans down clears the mask
	world_spans_init(FIX_W, FIX_H, FIX_TILE);
	assert(!world_spans_mask_loaded());
	fix_write_city(PATH);
	assert(world_spans_load(PATH));
}

// Each cell has its own ceiling: room for 50 in some columns, open in others
static void test_mask_gives_each_cell_its_own_ceiling(void) {
	static const mask_rec ramp[] = { { .tx = 12, .ty = 5, .levels = 1, .cols = { 0x00FF },
		                               .floor = { 16.0f },
		                               .clear = { { 25, 25, 25, 25 } } } };
	world_span s[WORLD_SPANS_MAX_PER_TILE];
	float tile12 = 12.0f * FIX_TILE;

	world_spans_set_mask_origin(0, 0);
	mask_write(MASK, 4, 12, 5, 1, 1, ramp, 1);
	assert(world_spans_load_mask(MASK));
	assert(world_spans_at(tile12 + 1.5f, Y5, s, WORLD_SPANS_MAX_PER_TILE) == 1);
	assert(s[0].floor == 16.0f && s[0].ceil == 66.0f);
	assert(world_spans_at(tile12 + 5.5f, Y5, s, WORLD_SPANS_MAX_PER_TILE) == 1);
	assert(s[0].ceil == WORLD_SPAN_OPEN_CEIL);
	// 50 of room is enough for a body of 40 and not for one of 60
	assert(world_spans_free_point(tile12 + 1.5f, Y5, 60.0f));
	assert(!world_spans_free_point(tile12 + 1.5f, Y5, 70.0f));
}

// A floor under a body above another cuts that span's ceiling at tile level;
// the mask gives the cells outside the upper floor their own, open, ceiling
static void test_a_cut_span_takes_its_cells_ceiling(void) {
	static const fix_record recs[] = { {FIX_T(40, 5), 0, 2} };
	static const world_span spans[] = { {0.0f, 39.0f}, {39.0f, FIX_OPEN} };
	// the rim laid over the face in columns 0-3, the ground in 4-15
	static const mask_rec mask[] = { { .tx = 40, .ty = 5, .levels = 2,
		                               .cols = { 0xFFF0, 0x000F }, .floor = { 0.0f, 39.0f } } };
	world_span s[WORLD_SPANS_MAX_PER_TILE];
	float tile40 = 40.0f * FIX_TILE;

	fix_write(PATH, FIX_W, FIX_H, FIX_TILE, recs, 1, spans, 2, WORLD_SPANS_VERSION, 0);
	assert(world_spans_load(PATH));
	world_spans_set_mask_origin(0, 0);
	mask_write(MASK, 4, 40, 5, 1, 1, mask, 1);
	assert(world_spans_load_mask(MASK));

	assert(world_spans_at(tile40 + 8.5f, Y5, s, WORLD_SPANS_MAX_PER_TILE) == 1);
	assert(s[0].floor == 0.0f && s[0].ceil == WORLD_SPAN_OPEN_CEIL);
	assert(world_spans_fits_box(tile40 + 10.0f, Y5, 6.0f, 0.0f));
	// over the face there is only the rim: no room at the ground
	assert(world_spans_at(tile40 + 1.5f, Y5, s, WORLD_SPANS_MAX_PER_TILE) == 1);
	assert(s[0].floor == 39.0f);
	assert(!world_spans_free_box(tile40 + 6.0f, Y5, 6.0f, 0.0f));
	assert(world_spans_free_box(tile40 + 6.0f, Y5, 6.0f, 39.0f));

	world_spans_init(FIX_W, FIX_H, FIX_TILE);
	fix_write_city(PATH);
	assert(world_spans_load(PATH));
}

static void test_mask_rejects_bad_files(void) {
	static const mask_rec half_ramp[] = { { .tx = 12, .ty = 5, .levels = 1, .cols = { 0x00FF }, .floor = { 16.0f } } };
	world_span s[WORLD_SPANS_MAX_PER_TILE];
	float tile12 = 12.0f * FIX_TILE;
	FILE* f;

	world_spans_set_mask_origin(0, 0);
	mask_write(MASK, 1, 12, 5, 1, 1, half_ramp, 1);
	assert(!world_spans_load_mask(MASK));
	mask_write(MASK, 2, 12, 5, 1, 1, half_ramp, 1);      // the old layout
	assert(!world_spans_load_mask(MASK));
	mask_write(MASK, 3, 12, 5, 1, 1, half_ramp, 1);      // no steep cells yet
	assert(!world_spans_load_mask(MASK));
	mask_write(MASK, 4, 12, 5, 1, 1, half_ramp, 1);
	mask_prefix(MASK, MASK_BAD, 200);
	assert(!world_spans_load_mask(MASK_BAD));
	assert(!world_spans_load_mask(DIR "missing.bin"));

	// a header claiming a grid the file does not hold
	f = fopen(MASK_BAD, "wb");
	fwrite("CFTG", 1, 4, f);
	mask_u32(f, 4u);
	mask_u32(f, 0u);
	mask_u32(f, 0u);
	mask_u32(f, 20000u);
	mask_u32(f, 20000u);
	mask_u32(f, 16u);
	mask_u32(f, 0u);
	fputc(0, f);
	fclose(f);
	assert(!world_spans_load_mask(MASK_BAD));
	// a map placed so far off that tile arithmetic would overflow
	mask_write(MASK, 4, 2147483000, 5, 1, 1, half_ramp, 1);
	assert(!world_spans_load_mask(MASK));

	// after a refusal there is no mask and every span is whole
	assert(!world_spans_mask_loaded());
	assert(world_spans_at(tile12 + 12.0f, Y5, s, WORLD_SPANS_MAX_PER_TILE) == 1);
}

int main(void) {
	mkdir(DIR, 0700);
	test_flat_default();
	world_spans_init(FIX_W, FIX_H, FIX_TILE);
	fix_write_city(PATH);
	assert(world_spans_load(PATH));
	test_ramp();
	test_walk_huge_distance();
	test_bridge();
	test_headroom_and_no_floor();
	test_free_and_landing();
	test_snap_for_login();
	test_partial_support();
	test_mask();
	test_mask_gives_each_cell_its_own_ceiling();
	test_a_cut_span_takes_its_cells_ceiling();
	test_mask_rejects_bad_files();
	test_loader_rejects_bad_files();
	world_spans_shutdown();
	assert(!world_spans_loaded());
	puts("world_spans_test: all tests passed");
	return 0;
}
