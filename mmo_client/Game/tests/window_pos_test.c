// Check how a window is placed and dragged by its title.

#include "ui/window_pos.h"

#include <assert.h>
#include <stdio.h>

#define VW 1920.0f
#define VH 1080.0f
#define W 400.0f
#define H 300.0f
#define STRIP_W 360.0f
#define STRIP_H 40.0f

static void origin(const window_pos* pos, float* x, float* y) {
	window_pos_origin(pos, VW, VH, W, H, x, y);
}

static int drag(window_pos* pos, float mx, float my, int clicked, int down) {
	return window_pos_drag(pos, VW, VH, W, H, STRIP_W, STRIP_H, mx, my, clicked, down);
}

static void test_a_new_window_is_centred(void) {
	window_pos pos;
	float x;
	float y;

	window_pos_reset(&pos);
	origin(&pos, &x, &y);
	assert(x == (VW - W) * 0.5f && y == (VH - H) * 0.5f);
}

static void test_a_press_on_the_title_picks_it_up_and_moving_carries_it(void) {
	window_pos pos;
	float x;
	float y;

	window_pos_reset(&pos);
	origin(&pos, &x, &y);

	assert(drag(&pos, x + 100.0f, y + 10.0f, 1, 1));
	assert(drag(&pos, x + 200.0f, y + 60.0f, 0, 1));
	origin(&pos, &x, &y);
	assert(x == (VW - W) * 0.5f + 100.0f);
	assert(y == (VH - H) * 0.5f + 50.0f);
}

static void test_letting_go_drops_it_where_it_is(void) {
	window_pos pos;
	float x;
	float y;
	float after_x;
	float after_y;

	window_pos_reset(&pos);
	origin(&pos, &x, &y);
	drag(&pos, x + 50.0f, y + 10.0f, 1, 1);
	drag(&pos, x + 150.0f, y + 110.0f, 0, 1);
	assert(!drag(&pos, x + 150.0f, y + 110.0f, 0, 0));
	origin(&pos, &after_x, &after_y);

	// Moving the mouse afterwards no longer drags it
	assert(!drag(&pos, 10.0f, 10.0f, 0, 0));
	origin(&pos, &x, &y);
	assert(x == after_x && y == after_y);
}

static void test_a_press_off_the_title_does_nothing(void) {
	window_pos pos;
	float x;
	float y;

	window_pos_reset(&pos);
	origin(&pos, &x, &y);
	assert(!drag(&pos, x + 100.0f, y + STRIP_H + 5.0f, 1, 1));
	assert(!drag(&pos, x + STRIP_W + 5.0f, y + 10.0f, 1, 1));
	assert(!drag(&pos, x - 5.0f, y + 10.0f, 1, 1));
	assert(!pos.dragging && !pos.placed);
}

static void test_holding_the_button_into_the_title_does_not_pick_it_up(void) {
	window_pos pos;
	float x;
	float y;

	window_pos_reset(&pos);
	origin(&pos, &x, &y);

	// Only the press picks it up, not a button already held while moving over it
	assert(!drag(&pos, x + 100.0f, y + 10.0f, 0, 1));
	assert(!pos.dragging);
}

static void test_it_cannot_be_dragged_off_the_screen(void) {
	window_pos pos;
	float x;
	float y;

	window_pos_reset(&pos);
	origin(&pos, &x, &y);
	drag(&pos, x + 10.0f, y + 10.0f, 1, 1);

	drag(&pos, -5000.0f, -5000.0f, 0, 1);
	origin(&pos, &x, &y);
	assert(x == 0.0f && y == 0.0f);

	drag(&pos, 9000.0f, 9000.0f, 0, 1);
	origin(&pos, &x, &y);
	assert(x == VW - W && y == VH - H);
}

static void test_a_window_bigger_than_the_view_stays_at_the_corner(void) {
	window_pos pos;
	float x;
	float y;

	window_pos_reset(&pos);
	window_pos_origin(&pos, 300.0f, 200.0f, 400.0f, 300.0f, &x, &y);
	window_pos_drag(&pos, 300.0f, 200.0f, 400.0f, 300.0f, 360.0f, 40.0f, 10.0f, 10.0f, 1, 1);
	window_pos_drag(&pos, 300.0f, 200.0f, 400.0f, 300.0f, 360.0f, 40.0f, 500.0f, 500.0f, 0, 1);
	window_pos_origin(&pos, 300.0f, 200.0f, 400.0f, 300.0f, &x, &y);
	assert(x == 0.0f && y == 0.0f);
}

static void test_a_moved_window_stays_when_the_view_changes_size(void) {
	window_pos pos;
	float x;
	float y;

	window_pos_reset(&pos);
	origin(&pos, &x, &y);
	drag(&pos, x + 10.0f, y + 10.0f, 1, 1);
	drag(&pos, x + 110.0f, y + 10.0f, 0, 1);
	drag(&pos, x + 110.0f, y + 10.0f, 0, 0);

	// A smaller view pulls it back inside rather than leaving it off screen
	window_pos_origin(&pos, 700.0f, 500.0f, W, H, &x, &y);
	assert(x >= 0.0f && x <= 700.0f - W && y >= 0.0f && y <= 500.0f - H);
}

static void test_reset_puts_it_back_in_the_middle(void) {
	window_pos pos;
	float x;
	float y;

	window_pos_reset(&pos);
	origin(&pos, &x, &y);
	drag(&pos, x + 10.0f, y + 10.0f, 1, 1);
	drag(&pos, x + 300.0f, y + 300.0f, 0, 1);
	window_pos_reset(&pos);
	origin(&pos, &x, &y);
	assert(x == (VW - W) * 0.5f && y == (VH - H) * 0.5f);
	assert(!pos.dragging && !pos.placed);
}

static void test_a_window_with_a_home_follows_it_until_moved(void) {
	window_pos pos;
	float x;
	float y;

	window_pos_reset(&pos);

	// A press that misses the title leaves it unplaced
	assert(!window_pos_drag_from(&pos, 100.0f, 200.0f, VW, VH, W, H, STRIP_W, STRIP_H, 500.0f,
	                             500.0f, 1, 1));
	assert(!pos.placed);

	// A press on the title at its home picks it up, measured from there
	assert(window_pos_drag_from(&pos, 100.0f, 200.0f, VW, VH, W, H, STRIP_W, STRIP_H, 150.0f,
	                            210.0f, 1, 1));
	assert(window_pos_drag_from(&pos, 100.0f, 200.0f, VW, VH, W, H, STRIP_W, STRIP_H, 250.0f,
	                            260.0f, 0, 1));
	window_pos_drag_from(&pos, 100.0f, 200.0f, VW, VH, W, H, STRIP_W, STRIP_H, 250.0f, 260.0f,
	                     0, 0);
	window_pos_origin(&pos, VW, VH, W, H, &x, &y);
	assert(x == 200.0f && y == 250.0f && pos.placed && !pos.dragging);
}

static void test_the_outline_is_a_band_around_the_border(void) {
	// A window at 100, 100, 200 by 100; a band of 10 reaches 5 either side
	assert(window_pos_on_edge(100.0f, 100.0f, 200.0f, 100.0f, 10.0f, 100.0f, 150.0f));
	assert(window_pos_on_edge(100.0f, 100.0f, 200.0f, 100.0f, 10.0f, 96.0f, 150.0f));
	assert(window_pos_on_edge(100.0f, 100.0f, 200.0f, 100.0f, 10.0f, 104.0f, 150.0f));
	assert(window_pos_on_edge(100.0f, 100.0f, 200.0f, 100.0f, 10.0f, 200.0f, 100.0f));
	assert(window_pos_on_edge(100.0f, 100.0f, 200.0f, 100.0f, 10.0f, 299.0f, 199.0f));

	// Well inside, well outside, and just past the band
	assert(!window_pos_on_edge(100.0f, 100.0f, 200.0f, 100.0f, 10.0f, 200.0f, 150.0f));
	assert(!window_pos_on_edge(100.0f, 100.0f, 200.0f, 100.0f, 10.0f, 50.0f, 150.0f));
	assert(!window_pos_on_edge(100.0f, 100.0f, 200.0f, 100.0f, 10.0f, 110.0f, 150.0f));
	assert(!window_pos_on_edge(100.0f, 100.0f, 200.0f, 100.0f, 10.0f, 90.0f, 150.0f));
}

static void test_carrying_by_the_outline(void) {
	window_pos pos;
	float x;
	float y;

	window_pos_reset(&pos);

	// A press that may not pick it up does nothing
	assert(!window_pos_carry(&pos, 100.0f, 200.0f, VW, VH, W, H, 0, 150.0f, 210.0f, 1));
	assert(!pos.placed && !pos.dragging);

	assert(window_pos_carry(&pos, 100.0f, 200.0f, VW, VH, W, H, 1, 150.0f, 210.0f, 1));
	assert(window_pos_carry(&pos, 100.0f, 200.0f, VW, VH, W, H, 0, 250.0f, 260.0f, 1));
	window_pos_carry(&pos, 100.0f, 200.0f, VW, VH, W, H, 0, 250.0f, 260.0f, 0);
	window_pos_origin(&pos, VW, VH, W, H, &x, &y);
	assert(x == 200.0f && y == 250.0f && !pos.dragging);
}

int main(void) {
	test_a_new_window_is_centred();
	test_a_press_on_the_title_picks_it_up_and_moving_carries_it();
	test_letting_go_drops_it_where_it_is();
	test_a_press_off_the_title_does_nothing();
	test_holding_the_button_into_the_title_does_not_pick_it_up();
	test_it_cannot_be_dragged_off_the_screen();
	test_a_window_bigger_than_the_view_stays_at_the_corner();
	test_a_moved_window_stays_when_the_view_changes_size();
	test_reset_puts_it_back_in_the_middle();
	test_a_window_with_a_home_follows_it_until_moved();
	test_the_outline_is_a_band_around_the_border();
	test_carrying_by_the_outline();
	printf("window_pos_test: all passed\n");
	return 0;
}
