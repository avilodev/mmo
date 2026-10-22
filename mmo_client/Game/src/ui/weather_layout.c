// Where everything in the weather window sits, in the menus' layout units.

#include "ui/weather_layout.h"

#define PAD 30.0f
#define CLOSE_SIZE 44.0f
#define PLACE_Y 70.0f
#define PLACE_H 50.0f
#define NOW_Y 130.0f
#define NOW_H 190.0f
#define ROWS_Y 335.0f
#define ROW_PITCH 68.0f
#define ROW_H 62.0f
#define FOOTER_Y 825.0f
#define FOOTER_H 45.0f
#define BAR_H 12.0f
#define BAR_MIN_W 6.0f

int weather_rect_has(const weather_rect* r, float x, float y) {
	return x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h;
}

weather_rect weather_panel(float frame_w, float frame_h) {
	weather_rect r = { (frame_w - WEATHER_PANEL_W) * 0.5f, (frame_h - WEATHER_PANEL_H) * 0.5f,
	                   WEATHER_PANEL_W, WEATHER_PANEL_H };

	return r;
}

weather_rect weather_close_button(const weather_rect* panel) {
	weather_rect r = { panel->x + panel->w - CLOSE_SIZE - 20.0f, panel->y + 17.0f, CLOSE_SIZE,
	                   CLOSE_SIZE };

	return r;
}

weather_rect weather_place_block(const weather_rect* panel) {
	weather_rect r = { panel->x + PAD, panel->y + PLACE_Y, panel->w - 2.0f * PAD, PLACE_H };

	return r;
}

weather_rect weather_now_block(const weather_rect* panel) {
	weather_rect r = { panel->x + PAD, panel->y + NOW_Y, panel->w - 2.0f * PAD, NOW_H };

	return r;
}

weather_rect weather_day_row(const weather_rect* panel, int ahead) {
	weather_rect r = { panel->x + PAD, panel->y + ROWS_Y + (float)ahead * ROW_PITCH,
	                   panel->w - 2.0f * PAD, ROW_H };

	return r;
}

// A box of a row, by its left edge and width, centred on the row's height
static weather_rect row_part(const weather_rect* row, float left, float w, float h) {
	weather_rect r = { row->x + left, row->y + (row->h - h) * 0.5f, w, h };

	return r;
}

weather_rect weather_row_icon(const weather_rect* row) {
	return row_part(row, 165.0f, 44.0f, 44.0f);
}

weather_rect weather_row_precip(const weather_rect* row) {
	return row_part(row, 217.0f, 66.0f, 30.0f);
}

weather_rect weather_row_lo(const weather_rect* row) {
	return row_part(row, 290.0f, 80.0f, 30.0f);
}

weather_rect weather_row_bar(const weather_rect* row) {
	return row_part(row, 382.0f, 190.0f, BAR_H);
}

weather_rect weather_row_hi(const weather_rect* row) {
	return row_part(row, 586.0f, 54.0f, 30.0f);
}

weather_rect weather_arrow_button(const weather_rect* panel) {
	weather_rect r = { panel->x + panel->w - 12.0f, panel->y + panel->h * 0.5f - 50.0f, 46.0f,
	                   100.0f };

	return r;
}

weather_rect weather_drawer_panel(const weather_rect* panel) {
	weather_rect r = { panel->x + panel->w, panel->y, WEATHER_DRAWER_W, panel->h };

	return r;
}

weather_rect weather_drawer_inner(const weather_rect* drawer) {
	weather_rect r = { drawer->x + PAD, drawer->y + PAD, drawer->w - 2.0f * PAD,
	                   drawer->h - 2.0f * PAD };

	return r;
}

weather_rect weather_drawer_recenter_button(const weather_rect* inner) {
	weather_rect r = { inner->x + inner->w - 114.0f, inner->y + inner->h - 58.0f, 100.0f, 44.0f };

	return r;
}

weather_rect weather_footer(const weather_rect* panel) {
	weather_rect r = { panel->x + PAD, panel->y + FOOTER_Y, panel->w - 2.0f * PAD, FOOTER_H };

	return r;
}

void weather_bar_span(const weather_rect* bar, int scale_lo, int scale_hi, int lo, int hi,
		float* out_x, float* out_w) {
	float range = (float)(scale_hi - scale_lo);
	float from = 0.0f;
	float to = 1.0f;
	float w;

	if(range > 0.0f) {
		from = (float)(lo - scale_lo) / range;
		to = (float)(hi - scale_lo) / range;
	}
	from = from < 0.0f ? 0.0f : (from > 1.0f ? 1.0f : from);
	to = to < 0.0f ? 0.0f : (to > 1.0f ? 1.0f : to);

	w = (to - from) * bar->w;
	if(w < BAR_MIN_W)
		w = BAR_MIN_W;
	if(from * bar->w + w > bar->w)
		from = (bar->w - w) / bar->w;

	*out_x = bar->x + from * bar->w;
	*out_w = w;
}
