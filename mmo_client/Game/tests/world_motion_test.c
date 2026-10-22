// Check the shared vertical motion: ramps, jumps, ledges, ceilings, blocks.

#define _POSIX_C_SOURCE 200809L
#include "world_spans_fixture.h"
#include "world_mask_fixture.h"
#include "world_motion.h"

#include <assert.h>
#include <sys/stat.h>
#include <math.h>
#include <stdio.h>

#define DIR "/tmp/world_motion_test_dir/"
#define PATH DIR "world_spans.bin"
#define MASK_PATH DIR "mask.bin"
#define HALF 6.0f
#define Y5 FIX_CX(5)

// Walk the box from x0 to x1 along row y over the given number of steps.
static int walk(vertical_state* s, float x0, float x1, float y, int steps) {
	for(int i = 1; i <= steps; i++) {
		float x = x0 + (x1 - x0) * (float)i / (float)steps;

		if(!vertical_step(s, x, y, HALF, 0))
			return 0;
	}
	return 1;
}

static void test_ramp(void) {
	vertical_state s = {0.0f, 0.0f, 1};

	assert(walk(&s, FIX_CX(10), FIX_CX(13), Y5, 45));
	assert(s.grounded && s.z == 24.0f);
	assert(walk(&s, FIX_CX(13), FIX_CX(10), Y5, 45));
	assert(s.grounded && s.z == 0.0f);
}

// Stand at (x, y), jump once, and report the apex and the steps in the air.
static void jump_at(float x, float y, float* apex, int* steps) {
	vertical_state s = {0.0f, 0.0f, 1};
	int i = 1;

	*apex = 0.0f;
	assert(vertical_step(&s, x, y, HALF, 1));
	while(i < 200 && !s.grounded) {
		if(s.z > *apex)
			*apex = s.z;
		assert(vertical_step(&s, x, y, HALF, 0));
		i++;
	}
	assert(s.grounded && s.z == 0.0f);
	*steps = i;
}

static void test_jump_on_flat_ground(void) {
	float apex;
	int steps;

	jump_at(FIX_CX(2), FIX_CX(2), &apex, &steps);
	assert(apex > 27.0f && apex < 31.0f);
	assert(steps > 40 && steps < 52);
}

// The deck leaves 70 over the ground and a body is 40, so a head stops at 30.
static void test_jump_under_a_deck_hits_it(void) {
	float apex;
	int steps;

	jump_at(FIX_CX(20), Y5, &apex, &steps);
	assert(apex <= 30.0f);
}

static void test_jump_needs_the_ground(void) {
	vertical_state s = {0.0f, 0.0f, 1};
	float first;

	assert(vertical_step(&s, FIX_CX(2), FIX_CX(2), HALF, 1));
	first = s.z;
	assert(!s.grounded && first > 0.0f);

	// a jump asked for in mid-air does nothing extra
	assert(vertical_step(&s, FIX_CX(2), FIX_CX(2), HALF, 1));
	assert(s.vz < WORLD_JUMP_SPEED - WORLD_GRAVITY * WORLD_VSTEP * 1.5f);
}

static void test_walk_off_a_platform_falls_and_lands(void) {
	vertical_state s = {100.0f, 0.0f, 1};

	assert(walk(&s, FIX_CX(29), FIX_CX(31), Y5, 16));
	assert(!s.grounded && s.z < 100.0f);
	for(int i = 0; i < 120 && !s.grounded; i++)
		assert(vertical_step(&s, FIX_CX(31), Y5, HALF, 0));
	assert(s.grounded && s.z == 0.0f && s.vz == 0.0f);
}

static void test_blocked_moves_leave_the_state_alone(void) {
	vertical_state s = {24.0f, 0.0f, 1};
	vertical_state before = s;

	// a 36 unit rise is a wall, not a step
	assert(!vertical_step(&s, FIX_CX(14), Y5, HALF, 0));
	assert(s.z == before.z && s.vz == before.vz && s.grounded == before.grounded);

	// no floor at all is a wall too
	s.z = 0.0f;
	assert(!vertical_step(&s, FIX_CX(24), Y5, HALF, 0));
	assert(!vertical_step(&s, NAN, Y5, HALF, 0));
	assert(!vertical_step(&s, FIX_CX(2), INFINITY, HALF, 0));
}

static void test_vertical_reach(void) {
	assert(world_vertical_ok(0.0f, 48.0f));
	assert(!world_vertical_ok(0.0f, 48.5f));
	assert(world_vertical_ok(100.0f, 60.0f));
	assert(world_vertical_ok(24.0f, 0.0f));
	assert(!world_vertical_ok(0.0f, 100.0f));
	assert(!world_vertical_ok(0.0f, NAN));
}

// Ledges as in the city: a slab corner at tile 41, a block corner at tile 45
static void write_ledges(void) {
	static const fix_record recs[] = {
		{FIX_T(40, 5), 0, 1}, {FIX_T(41, 5), 1, 2},
		{FIX_T(44, 5), 3, 1}, {FIX_T(45, 5), 4, 1},
	};
	static const world_span spans[] = {
		{0, FIX_OPEN}, {0, 96}, {106, FIX_OPEN}, {18, FIX_OPEN}, {39.3f, FIX_OPEN},
	};

	fix_write(PATH, FIX_W, FIX_H, FIX_TILE, recs, 4, spans, 5, WORLD_SPANS_VERSION, 0);
	assert(world_spans_load(PATH));
}

// Fall until grounded (at most 300 steps); returns 0 if a step was refused
static int fall(vertical_state* s, float x, int use_advance) {
	for(int i = 0; i < 300 && !s->grounded; i++) {
		int ok = use_advance ? vertical_advance(s, x, Y5, HALF, 0)
		                     : vertical_step(s, x, Y5, HALF, 0);

		if(!ok)
			return 0;
	}
	return 1;
}

static void test_falling_past_a_slab_corner_rests_on_it(void) {
	// centre over open ground, right corners over a slab 106 up
	float x = FIX_CX(40) + 6.0f;
	vertical_state s = {120.0f, -100.0f, 0};

	write_ledges();
	// the box overlaps the slab, so it rests on the top and is never refused
	assert(fall(&s, x, 0));
	assert(s.grounded && s.z == 106.0f);
	// walking away from the slab, the box clears it and falls to the ground
	for(float px = x; px > FIX_CX(40) - 8.0f; px -= 1.0f)
		assert(vertical_step(&s, px, Y5, HALF, 0));
	for(int i = 0; i < 300 && !(s.grounded && s.z == 0.0f); i++)
		assert(vertical_step(&s, FIX_CX(40) - 8.0f, Y5, HALF, 0));
	assert(s.grounded && s.z == 0.0f);
}

static void test_beside_a_block_corner_rests_on_it_then_falls_clear(void) {
	float x = FIX_CX(44) + 6.0f;
	vertical_state s = {60.0f, -100.0f, 0};

	write_ledges();
	// the corner over the 39.3 block holds the body up
	assert(fall(&s, x, 0));
	assert(s.grounded && s.z == 39.3f && s.vz == 0.0f);
	// stepping away from the block it settles on the floor under its centre
	for(float px = x; px > FIX_CX(44) - 2.0f; px -= 1.0f)
		assert(vertical_step(&s, px, Y5, HALF, 0));
	for(int i = 0; i < 300 && !(s.grounded && s.z == 18.0f); i++)
		assert(vertical_step(&s, FIX_CX(44) - 2.0f, Y5, HALF, 0));
	assert(s.grounded && s.z == 18.0f);
}

// A body already inside solid (placed there, not walked there) is stood up
static void test_a_body_inside_solid_is_stood_on_the_floor_below(void) {
	vertical_state in_slab = {80.0f, 0.0f, 0};
	vertical_state beside_block = {27.0f, 0.0f, 0};

	write_ledges();
	assert(!vertical_step(&in_slab, FIX_CX(40) + 6.0f, Y5, HALF, 0));
	assert(vertical_advance(&in_slab, FIX_CX(40) + 6.0f, Y5, HALF, 0));
	assert(in_slab.grounded && in_slab.z == 0.0f && in_slab.vz == 0.0f);
	assert(!vertical_step(&beside_block, FIX_CX(44) + 6.0f, Y5, HALF, 0));
	assert(vertical_advance(&beside_block, FIX_CX(44) + 6.0f, Y5, HALF, 0));
	assert(beside_block.grounded && beside_block.z == 18.0f);
}

static void test_advance_still_refuses_a_wall(void) {
	// stepping the state sideways into a ledge is a wall, for step and advance
	vertical_state s = {20.0f, -10.0f, 0};
	vertical_state copy = s;

	write_ledges();
	assert(!vertical_step(&copy, FIX_CX(45), Y5, HALF, 0));
	// a state that is not stuck at its own position is untouched by advance
	s.z = 60.0f;
	assert(vertical_advance(&s, FIX_CX(44), Y5, HALF, 0));
	assert(!s.grounded && s.z < 60.0f);
}

// Two centre floors; only the lower has room for the whole box
static void test_recovery_prefers_a_floor_the_box_fits_on(void) {
	static const fix_record recs[] = { {FIX_T(48, 5), 0, 2}, {FIX_T(49, 5), 2, 1} };
	static const world_span spans[] = { {0, 50}, {60, FIX_OPEN}, {0, 50} };
	float x = FIX_CX(48) + 6.0f;
	vertical_state s = {70.0f, 0.0f, 0};

	fix_write(PATH, FIX_W, FIX_H, FIX_TILE, recs, 2, spans, 3, WORLD_SPANS_VERSION, 0);
	assert(world_spans_load(PATH));
	assert(!vertical_step(&s, x, Y5, HALF, 0));
	assert(vertical_advance(&s, x, Y5, HALF, 0));
	assert(s.grounded && s.z == 0.0f);
}

// Walking off a platform keeps the body standing until the whole box is clear
static void test_standing_until_the_box_is_clear_of_the_edge(void) {
	vertical_state s = {100.0f, 0.0f, 1};
	float edge = FIX_CX(29) + 8.0f;

	fix_write_city(PATH);
	assert(world_spans_load(PATH));

	// the platform is tile 29 (floor 100); tile 30 beside it is open ground
	for(float x = FIX_CX(29); x < edge + 6.0f; x += 1.0f) {
		assert(vertical_step(&s, x, Y5, HALF, 0));
		assert(s.grounded && s.z == 100.0f);
	}
	// the box is now clear of the platform: it falls and lands, never refused
	for(int i = 0; i < 300 && !(s.grounded && s.z == 0.0f); i++)
		assert(vertical_step(&s, edge + 8.0f, Y5, HALF, 0));
	assert(s.grounded && s.z == 0.0f);
}

// A plateau ending mid-tile: the mask puts its edge at the cell
static void test_the_mask_puts_the_edge_at_the_cell(void) {
	static const fix_record recs[] = { {FIX_T(59, 5), 0, 1}, {FIX_T(60, 5), 1, 2} };
	static const world_span spans[] = { {100, FIX_OPEN}, {0, 100}, {100, FIX_OPEN} };
	// tile 60: the plateau in columns 0-7, the ground below it in columns 8-15
	static const mask_rec mask[] = { { .tx = 60, .ty = 5, .levels = 2, .cols = { 0xFF00, 0x00FF },
		                     .floor = { 0.0f, 100.0f } } };
	vertical_state s = {100.0f, 0.0f, 1};
	float edge = 60.0f * FIX_TILE + 8.0f;

	world_spans_init(FIX_W, FIX_H, FIX_TILE);
	fix_write(PATH, FIX_W, FIX_H, FIX_TILE, recs, 2, spans, 3, WORLD_SPANS_VERSION, 0);
	assert(world_spans_load(PATH));
	world_spans_set_mask_origin(0, 0);
	mask_write(MASK_PATH, 4, 59, 5, 3, 1, mask, 1);
	assert(world_spans_load_mask(MASK_PATH));

	// standing on the plateau until the whole box is past its cell edge
	for(float x = FIX_CX(59); x < edge + 6.0f; x += 1.0f) {
		assert(vertical_step(&s, x, Y5, HALF, 0));
		assert(s.grounded && s.z == 100.0f);
	}
	// then it falls through the open air beside the plateau to the ground
	for(int i = 0; i < 300 && !(s.grounded && s.z == 0.0f); i++)
		assert(vertical_step(&s, edge + 6.0f, Y5, HALF, 0));
	assert(s.grounded && s.z == 0.0f);

	// the edge is where the mask says: the plateau is gone beyond column 7
	assert(!world_spans_reach(edge + 1.0f, Y5, 100.0f, &s.z));
	world_spans_init(FIX_W, FIX_H, FIX_TILE);
	fix_write_city(PATH);
	assert(world_spans_load(PATH));
}

// A deck edge mid-tile with a slab under it: stepping off falls beside the slab
static void test_walking_off_a_deck_with_a_slab_under_it_falls(void) {
	static const fix_record recs[] = { {FIX_T(61, 5), 0, 1}, {FIX_T(62, 5), 1, 2} };
	// tile 61: the deck on solid; tile 62: ground under a slab, deck at 106
	static const world_span spans[] = { {106, FIX_OPEN}, {0, 96}, {106, FIX_OPEN} };
	static const mask_rec mask[] = { { .tx = 62, .ty = 5, .levels = 2, .cols = { 0xFFFF, 0x00FF },
		                               .floor = { 0.0f, 106.0f },
		                               .clear = { { 48, 48, 48, 48, 48, 48, 48, 48 } } } };
	vertical_state s = {106.0f, 0.0f, 1};
	float edge = 62.0f * FIX_TILE + 8.0f;

	world_spans_init(FIX_W, FIX_H, FIX_TILE);
	fix_write(PATH, FIX_W, FIX_H, FIX_TILE, recs, 2, spans, 3, WORLD_SPANS_VERSION, 0);
	assert(world_spans_load(PATH));
	world_spans_set_mask_origin(0, 0);
	mask_write(MASK_PATH, 4, 61, 5, 3, 1, mask, 1);
	assert(world_spans_load_mask(MASK_PATH));

	for(float x = FIX_CX(61); x < edge + 6.0f; x += 1.0f) {
		assert(vertical_step(&s, x, Y5, HALF, 0));
		assert(s.grounded && s.z == 106.0f);
	}
	for(int i = 0; i < 300 && !(s.grounded && s.z == 0.0f); i++)
		assert(vertical_step(&s, edge + 6.0f, Y5, HALF, 0));
	assert(s.grounded && s.z == 0.0f);

	world_spans_init(FIX_W, FIX_H, FIX_TILE);
	fix_write_city(PATH);
	assert(world_spans_load(PATH));
}

// A platform 39 up on tile 56; on tile 57 its rim is laid over its face in
// columns 0-3, and the ground it cuts to a 39 ceiling is open in columns 4-15
static void test_a_low_rim_over_its_face_beside_kept_ground(void) {
	static const fix_record recs[] = { {FIX_T(56, 5), 0, 1}, {FIX_T(57, 5), 1, 2} };
	static const world_span spans[] = { {39, FIX_OPEN}, {0, 39}, {39, FIX_OPEN} };
	static const mask_rec mask[] = { { .tx = 57, .ty = 5, .levels = 2,
		                               .cols = { 0xFFF0, 0x000F }, .floor = { 0.0f, 39.0f } } };
	float foot = 57.0f * FIX_TILE + 4.0f;
	vertical_state s = {39.0f, 0.0f, 1};

	world_spans_init(FIX_W, FIX_H, FIX_TILE);
	fix_write(PATH, FIX_W, FIX_H, FIX_TILE, recs, 2, spans, 3, WORLD_SPANS_VERSION, 0);
	assert(world_spans_load(PATH));
	world_spans_set_mask_origin(0, 0);
	mask_write(MASK_PATH, 4, 56, 5, 3, 1, mask, 1);
	assert(world_spans_load_mask(MASK_PATH));

	// off the top: standing to the face's foot, then falling to the ground
	for(float x = FIX_CX(56); x < foot + 6.0f; x += 1.0f) {
		assert(vertical_step(&s, x, Y5, HALF, 0));
		assert(s.grounded && s.z == 39.0f);
	}
	for(int i = 0; i < 300 && !(s.grounded && s.z == 0.0f); i++)
		assert(vertical_step(&s, foot + 6.0f, Y5, HALF, 0));
	assert(s.grounded && s.z == 0.0f);

	// from the ground the face is a wall
	assert(vertical_step(&s, foot + 6.0f, Y5, HALF, 0));
	assert(!vertical_step(&s, foot + 5.0f, Y5, HALF, 0));
	assert(s.grounded && s.z == 0.0f);

	world_spans_init(FIX_W, FIX_H, FIX_TILE);
	fix_write_city(PATH);
	assert(world_spans_load(PATH));
}

// A platform on tile 51 (solid below, floor at top) beside open ground, tile 50
static void write_platform(float top) {
	static const fix_record recs[] = { {FIX_T(50, 5), 0, 1}, {FIX_T(51, 5), 1, 1} };
	world_span spans[2] = { {0.0f, FIX_OPEN}, {0.0f, FIX_OPEN} };

	spans[1].floor = top;
	world_spans_init(FIX_W, FIX_H, FIX_TILE);
	fix_write(PATH, FIX_W, FIX_H, FIX_TILE, recs, 2, spans, 2, WORLD_SPANS_VERSION, 0);
	assert(world_spans_load(PATH));
}

// The box centre that leaves its right corners 4 units over the platform edge
#define OVER_EDGE (FIX_CX(50) + 6.0f)

// Back to the shared city fixture for the tests after these
static void restore_city(void) {
	world_spans_init(FIX_W, FIX_H, FIX_TILE);
	fix_write_city(PATH);
	assert(world_spans_load(PATH));
}

// A solid five columns wide in tile (40, 5), less than the box: walking along
// it head on, it would pass between two corners; the edge points catch it
static void test_a_solid_narrower_than_the_box_stops_it(void) {
	static const fix_record recs[] = { {FIX_T(40, 5), 0, 1} };
	static const world_span spans[] = { {0, FIX_OPEN} };
	static const mask_rec mask[] = { { .tx = 40, .ty = 5, .levels = 1, .cols = { 0xF83F },
		                               .floor = { 0.0f } } };
	vertical_state s = {0.0f, 0.0f, 1};
	float x = 40.0f * FIX_TILE + 8.5f;
	float y = FIX_CX(3);

	world_spans_init(FIX_W, FIX_H, FIX_TILE);
	fix_write(PATH, FIX_W, FIX_H, FIX_TILE, recs, 1, spans, 1, WORLD_SPANS_VERSION, 0);
	assert(world_spans_load(PATH));
	world_spans_set_mask_origin(0, 0);
	mask_write(MASK_PATH, 4, 40, 5, 1, 1, mask, 1);
	assert(world_spans_load_mask(MASK_PATH));

	while(y < FIX_CX(6) && vertical_step(&s, x, y + 1.0f, HALF, 0))
		y += 1.0f;
	assert(y + HALF < 5.0f * FIX_TILE);
	restore_city();
}

// Tile 46 is a 56-degree slope, too steep to walk: 1.5 up per column from the
// ground (tile 45) to 22.5, under a terrace at 24 on tile 47
static void write_steep_slope(void) {
	static const fix_record recs[] = { {FIX_T(46, 5), 0, 1}, {FIX_T(47, 5), 1, 1} };
	static const world_span spans[] = { {0, FIX_OPEN}, {24, FIX_OPEN} };
	static mask_rec mask[] = { { .tx = 46, .ty = 5, .levels = 1, .cols = { 0xFFFF },
		                         .floor = { 0.0f } } };

	for(int i = 0; i < 16; i++) {
		mask[0].delta[0][i] = (uint8_t)(i * 6);
		mask[0].clear[0][i] = 254;
	}
	world_spans_init(FIX_W, FIX_H, FIX_TILE);
	fix_write(PATH, FIX_W, FIX_H, FIX_TILE, recs, 2, spans, 2, WORLD_SPANS_VERSION, 0);
	assert(world_spans_load(PATH));
	world_spans_set_mask_origin(0, 0);
	mask_write(MASK_PATH, 4, 45, 5, 3, 1, mask, 1);
	assert(world_spans_load_mask(MASK_PATH));
}

// Walking into a slope too steep to walk stops at its foot
static void test_a_steep_slope_cannot_be_climbed(void) {
	vertical_state s = {0.0f, 0.0f, 1};
	float x = FIX_CX(45);

	write_steep_slope();
	while(vertical_step(&s, x + 1.0f, Y5, HALF, 0))
		x += 1.0f;
	assert(s.grounded && s.z == 0.0f);
	assert(x < 46.0f * FIX_TILE + 2.0f);
	assert(world_spans_steep_above(x + 1.0f, Y5, s.z));
	restore_city();
}

// Coming off the terrace the body stands on the slope all the way down, and
// slides downhill wherever it stands on it
static void test_a_steep_slope_is_gone_down_and_slides(void) {
	vertical_state s = {24.0f, 0.0f, 1};
	float dir_x = 0.0f;
	float dir_y = 0.0f;
	float grade = 0.0f;
	float last = 24.0f;

	write_steep_slope();
	for(float x = FIX_CX(47); x > FIX_CX(45); x -= 1.0f) {
		for(int i = 0; i < 300 && !s.grounded; i++)
			assert(vertical_step(&s, x, Y5, HALF, 0));
		assert(vertical_step(&s, x, Y5, HALF, 0));
		assert(s.z <= last);
		last = s.z;
	}
	assert(s.grounded && s.z == 0.0f);

	s.z = 12.0f;
	assert(world_spans_slide(46.0f * FIX_TILE + 8.5f, Y5, 12.0f, &dir_x, &dir_y, &grade));
	assert(dir_x < -0.99f && fabsf(dir_y) < 0.01f);
	assert(fabsf(grade - 1.5f) < 0.01f);
	assert(!world_spans_slide(FIX_CX(45), Y5, 0.0f, &dir_x, &dir_y, NULL));
	assert(!world_spans_slide(FIX_CX(47), Y5, 24.0f, &dir_x, &dir_y, NULL));
	restore_city();
}

// A steep slope whose first cell is only a lip: 0.25 up, then 1.25 a column
static void write_steep_slope_with_lip(void) {
	static const fix_record recs[] = { {FIX_T(46, 5), 0, 1}, {FIX_T(47, 5), 1, 1} };
	static const world_span spans[] = { {0, FIX_OPEN}, {24, FIX_OPEN} };
	static mask_rec mask[] = { { .tx = 46, .ty = 5, .levels = 1, .cols = { 0xFFFF },
		                         .floor = { 0.0f } } };

	for(int i = 0; i < 16; i++) {
		mask[0].delta[0][i] = (uint8_t)(i == 0 ? 1 : 5 * i + 1);
		mask[0].clear[0][i] = 254;
	}
	world_spans_init(FIX_W, FIX_H, FIX_TILE);
	fix_write(PATH, FIX_W, FIX_H, FIX_TILE, recs, 2, spans, 2, WORLD_SPANS_VERSION, 0);
	assert(world_spans_load(PATH));
	world_spans_set_mask_origin(0, 0);
	mask_write(MASK_PATH, 4, 45, 5, 3, 1, mask, 1);
	assert(world_spans_load_mask(MASK_PATH));
}

// The lip at the foot of a steep slope is not the slope: standing on it the
// body stops at the slope rather than being pushed off it and walking back on
static void test_the_lip_of_a_steep_slope_does_not_slide(void) {
	float dir_x = 0.0f;
	float dir_y = 0.0f;
	float grade = 0.0f;

	write_steep_slope_with_lip();
	assert(!world_spans_slide(46.0f * FIX_TILE + 0.5f, Y5, 0.25f, &dir_x, &dir_y, &grade));
	assert(world_spans_slide(46.0f * FIX_TILE + 8.5f, Y5, 10.25f, &dir_x, &dir_y, &grade));
	assert(fabsf(grade - 1.25f) < 0.01f);
	restore_city();
}

// Overlapping a ledge's face while falling must never sink the body into it
static void test_falling_beside_a_ledge_face_never_sinks_into_it(void) {
	vertical_state s = {30.0f, -10.0f, 0};

	write_platform(40.0f);
	for(int i = 0; i < 60 && !s.grounded; i++) {
		assert(vertical_step(&s, OVER_EDGE, Y5, HALF, 0));
		assert(s.z >= 29.0f);
	}
	assert(s.grounded && s.z == 40.0f && s.vz == 0.0f);
	restore_city();
}

// Falling clear of a face, the whole way down, is never refused
static void test_falling_clear_along_a_ledge_face_lands_below(void) {
	vertical_state s = {40.0f, 0.0f, 0};
	float last = s.z;

	write_platform(40.0f);
	for(int i = 0; i < 300 && !s.grounded; i++) {
		assert(vertical_step(&s, FIX_CX(50), Y5, HALF, 0));
		assert(s.z <= last);
		last = s.z;
	}
	assert(s.grounded && s.z == 0.0f);
	restore_city();
}

// Step off, then turn back into the ledge: the body is never stuck or refused
static void test_turning_back_into_the_ledge_while_falling_is_never_refused(void) {
	vertical_state s = {40.0f, 0.0f, 1};
	float x;

	write_platform(40.0f);
	for(x = FIX_CX(51); x > FIX_CX(50); x -= 1.0f)
		assert(vertical_step(&s, x, Y5, HALF, 0));
	assert(!s.grounded);
	for(int i = 0; i < 6; i++)
		assert(vertical_step(&s, FIX_CX(50), Y5, HALF, 0));
	for(x = FIX_CX(50); x < OVER_EDGE + 2.0f; x += 0.5f)
		assert(vertical_step(&s, x, Y5, HALF, 0));
	for(int i = 0; i < 120; i++)
		assert(vertical_step(&s, OVER_EDGE + 2.0f, Y5, HALF, 0));
	assert(s.grounded && s.z == 40.0f);
	restore_city();
}

// A face taller than a step above the feet is a wall for a falling body
static void test_a_face_above_the_feet_is_a_wall_while_falling(void) {
	vertical_state s = {20.0f, 0.0f, 0};
	vertical_state before = s;

	write_platform(40.0f);
	assert(!vertical_step(&s, OVER_EDGE, Y5, HALF, 0));
	assert(s.z == before.z && s.vz == before.vz && s.grounded == before.grounded);
	restore_city();
}

// A jump that rises beside a ledge and tops out against it stands on it
static void test_a_jump_beside_a_ledge_ends_on_it(void) {
	vertical_state s = {0.0f, 0.0f, 1};

	write_platform(30.0f);
	assert(vertical_step(&s, FIX_CX(50), Y5, HALF, 1));
	for(int i = 0; i < 120 && !s.grounded; i++) {
		vertical_state moved = s;

		// lean into the ledge whenever the box is allowed to
		if(vertical_step(&moved, OVER_EDGE, Y5, HALF, 0))
			s = moved;
		else
			assert(vertical_step(&s, FIX_CX(50), Y5, HALF, 0));
	}
	assert(s.grounded && s.z == 30.0f);
	restore_city();
}

static void test_advance_ignores_bad_input(void) {
	vertical_state s = {NAN, 0.0f, 0};

	write_ledges();
	assert(!vertical_advance(&s, FIX_CX(44), Y5, HALF, 0));
	s.z = 60.0f;
	assert(!vertical_advance(&s, NAN, Y5, HALF, 0));
	assert(s.z == 60.0f);
}

int main(void) {
	mkdir(DIR, 0700);
	world_spans_init(FIX_W, FIX_H, FIX_TILE);
	fix_write_city(PATH);
	assert(world_spans_load(PATH));
	test_ramp();
	test_jump_on_flat_ground();
	test_jump_under_a_deck_hits_it();
	test_jump_needs_the_ground();
	test_walk_off_a_platform_falls_and_lands();
	test_blocked_moves_leave_the_state_alone();
	test_vertical_reach();
	test_falling_past_a_slab_corner_rests_on_it();
	test_beside_a_block_corner_rests_on_it_then_falls_clear();
	test_a_body_inside_solid_is_stood_on_the_floor_below();
	test_advance_still_refuses_a_wall();
	test_recovery_prefers_a_floor_the_box_fits_on();
	test_standing_until_the_box_is_clear_of_the_edge();
	test_the_mask_puts_the_edge_at_the_cell();
	test_walking_off_a_deck_with_a_slab_under_it_falls();
	test_a_low_rim_over_its_face_beside_kept_ground();
	test_a_solid_narrower_than_the_box_stops_it();
	test_a_steep_slope_cannot_be_climbed();
	test_a_steep_slope_is_gone_down_and_slides();
	test_the_lip_of_a_steep_slope_does_not_slide();
	test_falling_beside_a_ledge_face_never_sinks_into_it();
	test_falling_clear_along_a_ledge_face_lands_below();
	test_turning_back_into_the_ledge_while_falling_is_never_refused();
	test_a_face_above_the_feet_is_a_wall_while_falling();
	test_a_jump_beside_a_ledge_ends_on_it();
	test_advance_ignores_bad_input();
	world_spans_shutdown();
	puts("world_motion_test: all tests passed");
	return 0;
}
