// Check the eased height never overshoots and snaps across big gaps.
#include "render/height_smooth.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>

int main(void) {
	float z = 0.0f;

	for(int i = 0; i < 120; i++) {
		float next = height_smooth_step(z, 24.0f, 1.0f / 60.0f);

		assert(next >= z && next <= 24.0f);
		z = next;
	}
	assert(fabsf(z - 24.0f) < 0.01f);
	assert(height_smooth_step(0.0f, 200.0f, 1.0f / 60.0f) == 200.0f);
	assert(height_smooth_step(NAN, 5.0f, 0.016f) == 5.0f);
	assert(height_smooth_step(1.0f, 5.0f, NAN) == 5.0f);

	// a long frame lands on the target rather than past it
	assert(height_smooth_step(0.0f, 10.0f, 5.0f) == 10.0f);
	// time running backwards does not move it
	assert(height_smooth_step(3.0f, 10.0f, -1.0f) == 3.0f);
	// descending eases down too
	float down = height_smooth_step(24.0f, 0.0f, 1.0f / 60.0f);

	assert(down < 24.0f && down >= 0.0f);
	puts("height_smooth_test: all tests passed");
	return 0;
}
