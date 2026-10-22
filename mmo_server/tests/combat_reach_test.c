// The shape combat's vertical gates rely on: horizontal reach says nothing
// about height, so a target far above a circle is inside it but out of reach.

#include "npc_geometry.h"
#include "world_motion.h"

#include <assert.h>
#include <stdio.h>

int main(void) {
	// Same x/y as the circle centre, so the horizontal test passes.
	assert(point_in_circle(10.0f, 10.0f, 10.0f, 10.0f, 30.0f));

	// 100 units above: inside the circle, out of vertical reach.
	assert(point_in_circle(10.0f, 10.0f, 10.0f, 10.0f, 30.0f));
	assert(!world_vertical_ok(0.0f, 100.0f));
	assert(!world_vertical_ok(100.0f, 0.0f));

	// 24 units above: both.
	assert(point_in_circle(10.0f, 10.0f, 10.0f, 10.0f, 30.0f));
	assert(world_vertical_ok(0.0f, 24.0f));
	assert(world_vertical_ok(24.0f, 0.0f));

	// Edge of reach.
	assert(world_vertical_ok(0.0f, WORLD_VREACH));
	assert(!world_vertical_ok(0.0f, WORLD_VREACH + 1.0f));

	printf("combat_reach_test: ok\n");
	return 0;
}
