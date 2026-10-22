// Check where the weather window puts things: inside it and not overlapping

#include "ui/weather_layout.h"

#include <assert.h>
#include <stdio.h>

static int inside(const weather_rect* outer, const weather_rect* r) {
	return r->x >= outer->x && r->y >= outer->y && r->x + r->w <= outer->x + outer->w &&
	       r->y + r->h <= outer->y + outer->h;
}

static int overlaps(const weather_rect* a, const weather_rect* b) {
	return a->x < b->x + b->w && b->x < a->x + a->w && a->y < b->y + b->h && b->y < a->y + a->h;
}

static void test_the_window_is_centred(void) {
	weather_rect p = weather_panel(3200.0f, 1800.0f);

	assert(p.x == (3200.0f - WEATHER_PANEL_W) * 0.5f);
	assert(p.y == (1800.0f - WEATHER_PANEL_H) * 0.5f);
	assert(p.w == WEATHER_PANEL_W && p.h == WEATHER_PANEL_H);
}

static void test_the_window_fits_the_shortest_screen(void) {
	// The layout is always 1800 units high, however the window is shaped
	weather_rect p = weather_panel(2400.0f, 1800.0f);

	assert(p.y >= 0.0f && p.y + p.h <= 1800.0f);
	assert(p.x >= 0.0f && p.x + p.w <= 2400.0f);
}

static void test_the_close_button_is_in_the_corner(void) {
	weather_rect p = weather_panel(3200.0f, 1800.0f);
	weather_rect close = weather_close_button(&p);

	assert(inside(&p, &close));
	assert(close.x > p.x + p.w * 0.8f && close.y < p.y + 100.0f);
}

static void test_the_blocks_do_not_overlap(void) {
	weather_rect p = weather_panel(3200.0f, 1800.0f);
	weather_rect place = weather_place_block(&p);
	weather_rect now = weather_now_block(&p);
	weather_rect first = weather_day_row(&p, 0);
	weather_rect foot = weather_footer(&p);

	assert(inside(&p, &place) && inside(&p, &now) && inside(&p, &first) && inside(&p, &foot));
	assert(!overlaps(&place, &now));
	assert(!overlaps(&now, &first));
	assert(!overlaps(&first, &foot));
}

static void test_seven_rows_stack_inside_the_window(void) {
	weather_rect p = weather_panel(3200.0f, 1800.0f);
	weather_rect foot = weather_footer(&p);

	for(int ahead = 0; ahead < WEATHER_FORECAST_DAYS; ahead++) {
		weather_rect row = weather_day_row(&p, ahead);

		assert(inside(&p, &row));
		assert(!overlaps(&row, &foot));
		if(ahead > 0) {
			weather_rect before = weather_day_row(&p, ahead - 1);

			assert(!overlaps(&before, &row));
			assert(row.y > before.y);
		}
	}
}

static void test_a_rows_parts_sit_in_order_without_overlap(void) {
	weather_rect p = weather_panel(3200.0f, 1800.0f);
	weather_rect row = weather_day_row(&p, 3);
	weather_rect icon = weather_row_icon(&row);
	weather_rect precip = weather_row_precip(&row);
	weather_rect lo = weather_row_lo(&row);
	weather_rect bar = weather_row_bar(&row);
	weather_rect hi = weather_row_hi(&row);

	assert(inside(&row, &icon) && inside(&row, &precip) && inside(&row, &lo));
	assert(inside(&row, &bar) && inside(&row, &hi));
	assert(!overlaps(&icon, &precip) && !overlaps(&precip, &lo));
	assert(!overlaps(&lo, &bar) && !overlaps(&bar, &hi));
	assert(icon.x < precip.x && precip.x < lo.x && lo.x < bar.x && bar.x < hi.x);
}

static void test_the_drawer_sits_against_the_window_and_the_arrow_stays_on_the_seam(void) {
	weather_rect p = weather_panel(3200.0f, 1800.0f);
	weather_rect drawer = weather_drawer_panel(&p);
	weather_rect inner = weather_drawer_inner(&drawer);
	weather_rect button = weather_drawer_recenter_button(&inner);
	weather_rect arrow = weather_arrow_button(&p);

	assert(drawer.x == p.x + p.w && drawer.y == p.y && drawer.h == p.h);
	assert(drawer.w == WEATHER_DRAWER_W);
	assert(inside(&drawer, &inner));
	assert(inside(&inner, &button));
	assert(arrow.x < drawer.x && arrow.x + arrow.w > drawer.x);
	assert(!overlaps(&arrow, &button));
}

static void test_a_bar_span_follows_the_degrees(void) {
	weather_rect bar = { 100.0f, 0.0f, 400.0f, 20.0f };
	float x;
	float w;

	// The whole scale fills the whole bar
	weather_bar_span(&bar, -10, 30, -10, 30, &x, &w);
	assert(x == 100.0f && w == 400.0f);

	// The middle quarter of the scale
	weather_bar_span(&bar, 0, 40, 10, 20, &x, &w);
	assert(x == 200.0f && w == 100.0f);

	// A day with one temperature still shows a mark
	weather_bar_span(&bar, 0, 40, 20, 20, &x, &w);
	assert(w >= 6.0f && x >= bar.x && x + w <= bar.x + bar.w);

	// A flat scale does not divide by zero
	weather_bar_span(&bar, 5, 5, 5, 5, &x, &w);
	assert(w > 0.0f && x >= bar.x && x + w <= bar.x + bar.w);

	// Days off the scale are held to the bar
	weather_bar_span(&bar, 0, 10, -50, 50, &x, &w);
	assert(x == 100.0f && w == 400.0f);
}

static void test_the_arrow_sits_on_the_right_edge_clear_of_the_close_button(void) {
	weather_rect p = weather_panel(3200.0f, 1800.0f);
	weather_rect close = weather_close_button(&p);
	weather_rect arrow = weather_arrow_button(&p);

	// Half over the edge, in the middle of its height, and not on the close button
	assert(arrow.x < p.x + p.w && arrow.x + arrow.w > p.x + p.w);
	assert(arrow.y > p.y + p.h * 0.3f && arrow.y + arrow.h < p.y + p.h * 0.7f);
	assert(!overlaps(&arrow, &close));
}

int main(void) {
	test_the_window_is_centred();
	test_the_window_fits_the_shortest_screen();
	test_the_close_button_is_in_the_corner();
	test_the_blocks_do_not_overlap();
	test_seven_rows_stack_inside_the_window();
	test_a_rows_parts_sit_in_order_without_overlap();
	test_the_arrow_sits_on_the_right_edge_clear_of_the_close_button();
	test_the_drawer_sits_against_the_window_and_the_arrow_stays_on_the_seam();
	test_a_bar_span_follows_the_degrees();
	printf("weather_layout_test: all passed\n");
	return 0;
}
