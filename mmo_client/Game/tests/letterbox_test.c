/**
 * @file
 * Check the 16:9 box the game draws into and the cursor mapping through it.
 */
#include "render/letterbox.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>

static void test_exact_fit(void) {
	letterbox_rect r = letterbox_fit(1920, 1080, 1920, 1080);

	assert(r.x == 0 && r.y == 0 && r.w == 1920 && r.h == 1080);
	r = letterbox_fit(2560, 1440, 1920, 1080);
	assert(r.x == 0 && r.y == 0 && r.w == 2560 && r.h == 1440);
}

static void test_wide_window_gets_side_bars(void) {
	// 21:9 monitor
	letterbox_rect r = letterbox_fit(3440, 1440, 1920, 1080);

	assert(r.h == 1440 && r.y == 0);
	assert(r.w == 2560 && r.x == 440);
}

static void test_tall_window_gets_top_and_bottom_bars(void) {
	// 16:10 monitor
	letterbox_rect r = letterbox_fit(1920, 1200, 1920, 1080);

	assert(r.w == 1920 && r.x == 0);
	assert(r.h == 1080 && r.y == 60);
}

static void test_never_stretches(void) {
	int sizes[][2] = { {1366, 768}, {1000, 1000}, {800, 1200}, {5120, 1440}, {1281, 721} };

	for(int i = 0; i < 5; i++) {
		letterbox_rect r = letterbox_fit(sizes[i][0], sizes[i][1], 1920, 1080);

		assert(r.w <= sizes[i][0] && r.h <= sizes[i][1]);
		// aspect within a pixel of 16:9
		assert(fabs((double)r.w * 1080.0 - (double)r.h * 1920.0) <= 1920.0);
	}
}

static void test_degenerate_sizes(void) {
	letterbox_rect r = letterbox_fit(0, 0, 1920, 1080);

	assert(r.w == 0 && r.h == 0);
	r = letterbox_fit(800, 600, 0, 0);
	assert(r.w == 800 && r.h == 600);
}

static void test_cursor_maps_into_the_view(void) {
	letterbox_rect r = letterbox_fit(3440, 1440, 1920, 1080);
	float x, y;

	// top-left and bottom-right of the box are the view corners
	letterbox_to_view(&r, r.x, r.y, 1920, 1080, &x, &y);
	assert(fabsf(x) < 0.01f && fabsf(y) < 0.01f);
	letterbox_to_view(&r, r.x + r.w, r.y + r.h, 1920, 1080, &x, &y);
	assert(fabsf(x - 1920.0f) < 0.01f && fabsf(y - 1080.0f) < 0.01f);

	// the middle of the window is the middle of the view
	letterbox_to_view(&r, 1720.0, 720.0, 1920, 1080, &x, &y);
	assert(fabsf(x - 960.0f) < 0.01f && fabsf(y - 540.0f) < 0.01f);

	// a cursor on a bar maps outside the view
	letterbox_to_view(&r, 10.0, 720.0, 1920, 1080, &x, &y);
	assert(x < 0.0f);
}

int main(void) {
	test_exact_fit();
	test_wide_window_gets_side_bars();
	test_tall_window_gets_top_and_bottom_bars();
	test_never_stretches();
	test_degenerate_sizes();
	test_cursor_maps_into_the_view();
	printf("letterbox_test: OK\n");

	return 0;
}
