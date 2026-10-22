// Check the client's use of the shared vertical motion.

#define _POSIX_C_SOURCE 200809L
#include "world_spans_fixture.h"
#include "world_motion.h"
#include "player/player_vertical.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>

#define HALF 6.0f
#define Y5 FIX_CX(5)

static void test_probe(void) {
	vertical_state on_ramp = {24.0f, 0.0f, 1};

	assert(vertical_probe(&on_ramp, FIX_CX(12), Y5, HALF));
	assert(!vertical_probe(&on_ramp, FIX_CX(14), Y5, HALF));   // taller than a step
	assert(!vertical_probe(&on_ramp, FIX_CX(24), Y5, HALF));   // no floor
	assert(on_ramp.z == 24.0f && on_ramp.grounded);            // a probe changes nothing
}

// Run frames of dt until the body lands; return the frames it took.
static int fly(vertical_state* s, vertical_clock* c, float dt, int* jumped_total) {
	int frames = 0;
	int jumped = 0;

	while(frames < 400) {
		vertical_update(s, c, FIX_CX(2), FIX_CX(2), HALF, dt, 0, &jumped);
		*jumped_total += jumped;
		frames++;
		if(s->grounded)
			break;
	}
	return frames;
}

static void test_jump_takes_the_same_time_at_any_frame_rate(void) {
	for(int pass = 0; pass < 2; pass++) {
		float dt = pass ? 1.0f / 30.0f : 1.0f / 60.0f;
		vertical_state s = {0.0f, 0.0f, 1};
		vertical_clock c = {0.0f, 0.0f};
		int jumped = 0;
		int total = 0;

		vertical_update(&s, &c, FIX_CX(2), FIX_CX(2), HALF, dt, 1, &jumped);
		assert(jumped == 1 && !s.grounded);
		total += jumped;

		float seconds = (float)fly(&s, &c, dt, &total) * dt;

		assert(s.grounded && s.z == 0.0f);
		assert(seconds > 0.70f && seconds < 0.85f);   // about 0.76 s
		assert(total == 1);                           // one press, one jump
	}
}

static void test_a_press_just_before_landing_still_jumps(void) {
	vertical_state s = {5.0f, -150.0f, 0};     // about to land
	vertical_clock c = {0.0f, 0.0f};
	int jumped = 0;

	vertical_update(&s, &c, FIX_CX(2), FIX_CX(2), HALF, 1.0f / 60.0f, 1, &jumped);
	for(int i = 0; i < 10 && !jumped; i++)
		vertical_update(&s, &c, FIX_CX(2), FIX_CX(2), HALF, 1.0f / 60.0f, 0, &jumped);
	assert(jumped == 1);
}

static void test_a_huge_frame_is_capped(void) {
	vertical_state s = {0.0f, 0.0f, 1};
	vertical_clock c = {0.0f, 0.0f};
	int jumped = 0;

	// a one second stall may not fast-forward more than a quarter second
	vertical_update(&s, &c, FIX_CX(2), FIX_CX(2), HALF, 1.0f, 1, &jumped);
	assert(!s.grounded && s.z > 5.0f && s.z < 31.0f);
}

int main(void) {
	world_spans_init(FIX_W, FIX_H, FIX_TILE);
	fix_write_city("/tmp/player_vertical_test.bin");
	assert(world_spans_load("/tmp/player_vertical_test.bin"));
	test_probe();
	test_jump_takes_the_same_time_at_any_frame_rate();
	test_a_press_just_before_landing_still_jumps();
	test_a_huge_frame_is_capped();
	world_spans_shutdown();
	puts("player_vertical_test: all tests passed");
	return 0;
}
