// Check ground picking against flat ground, a ramp top, and the world edge.

#define _POSIX_C_SOURCE 200809L
#include "world_spans_fixture.h"
#include "world/world_pick.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>

int main(void) {
	float out[3];
	float down[3] = {0.0f, 0.0f, -1.0f};
	float diag[3] = {0.7071f, 0.0f, -0.7071f};

	world_spans_init(FIX_W, FIX_H, FIX_TILE);
	fix_write_city("/tmp/world_pick_test.bin");
	assert(world_spans_load("/tmp/world_pick_test.bin"));

	// straight down onto flat ground
	float o1[3] = {FIX_CX(2), FIX_CX(2), 200.0f};
	assert(world_pick_ground(o1, down, 500.0f, out));
	assert(fabsf(out[2]) < 0.1f && fabsf(out[0] - FIX_CX(2)) < 0.1f);

	// straight down onto the top of the ramp at tile 13
	float o2[3] = {FIX_CX(13), FIX_CX(5), 200.0f};
	assert(world_pick_ground(o2, down, 500.0f, out));
	assert(fabsf(out[2] - 24.0f) < 0.1f);

	// a slanted ray lands where geometry says (z falls as x rises)
	float o3[3] = {FIX_CX(2), FIX_CX(2), 100.0f};
	assert(world_pick_ground(o3, diag, 500.0f, out));
	assert(fabsf(out[2]) < 0.2f && fabsf(out[0] - (FIX_CX(2) + 100.0f)) < 1.0f);

	// out of reach, off the world, and starting inside solid all return 0
	assert(!world_pick_ground(o1, down, 50.0f, out));
	float o4[3] = {-50.0f, FIX_CX(2), 100.0f};
	assert(!world_pick_ground(o4, down, 500.0f, out));
	float o5[3] = {FIX_CX(24), FIX_CX(5), 100.0f};   // a tile with no floor
	assert(!world_pick_ground(o5, down, 500.0f, out));

	world_spans_shutdown();
	puts("world_pick_test: all tests passed");
	return 0;
}
