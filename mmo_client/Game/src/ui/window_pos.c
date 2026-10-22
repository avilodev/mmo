// Where a window sits, and dragging it by its title.

#include "ui/window_pos.h"

#include <string.h>

// A value held between low and high, where high wins if the range is empty
static float clamp_to(float value, float low, float high) {
	if(high < low)
		return low;
	if(value < low)
		return low;

	return value > high ? high : value;
}

void window_pos_reset(window_pos* pos) {
	memset(pos, 0, sizeof(*pos));
}

void window_pos_origin(const window_pos* pos, float view_w, float view_h, float w, float h,
                       float* out_x, float* out_y) {
	float x = (view_w - w) * 0.5f;
	float y = (view_h - h) * 0.5f;

	if(pos->placed) {
		x = pos->x;
		y = pos->y;
	}

	*out_x = clamp_to(x, 0.0f, view_w - w);
	*out_y = clamp_to(y, 0.0f, view_h - h);
}

// Whether a point is in a rectangle
static int in_rect(float x, float y, float w, float h, float px, float py) {
	return px >= x && px < x + w && py >= y && py < y + h;
}

int window_pos_on_edge(float x, float y, float w, float h, float band, float mx, float my) {
	float inner = band * 0.5f;

	// Within the band either side of the outline, but not out in the middle
	if(!in_rect(x - inner, y - inner, w + band, h + band, mx, my))
		return 0;

	return !in_rect(x + inner, y + inner, w - band, h - band, mx, my);
}

int window_pos_carry(window_pos* pos, float home_x, float home_y, float view_w, float view_h,
                     float w, float h, int may_pick, float mx, float my, int down) {
	window_pos moved = *pos;
	float x;
	float y;

	// Measure the grab from where the window is drawn
	if(!moved.placed) {
		moved.x = home_x;
		moved.y = home_y;
		moved.placed = 1;
	}
	window_pos_origin(&moved, view_w, view_h, w, h, &x, &y);

	if(!moved.dragging) {
		if(!may_pick)
			return 0;

		moved.dragging = 1;
		moved.grab_x = mx - x;
		moved.grab_y = my - y;
	}

	if(!down) {
		moved.dragging = 0;
		*pos = moved;
		return 0;
	}

	moved.x = clamp_to(mx - moved.grab_x, 0.0f, view_w - w);
	moved.y = clamp_to(my - moved.grab_y, 0.0f, view_h - h);
	*pos = moved;

	return 1;
}

int window_pos_drag_from(window_pos* pos, float home_x, float home_y, float view_w,
                         float view_h, float w, float h, float strip_w, float strip_h, float mx,
                         float my, int clicked, int down) {
	float x = home_x;
	float y = home_y;

	if(pos->placed)
		window_pos_origin(pos, view_w, view_h, w, h, &x, &y);

	// A press that misses the strip leaves an unmoved window unplaced
	if(!pos->dragging && !(clicked && in_rect(x, y, strip_w, strip_h, mx, my)))
		return 0;

	return window_pos_carry(pos, home_x, home_y, view_w, view_h, w, h, 1, mx, my, down);
}

int window_pos_drag(window_pos* pos, float view_w, float view_h, float w, float h,
                    float strip_w, float strip_h, float mx, float my, int clicked, int down) {
	float x;
	float y;

	window_pos_origin(pos, view_w, view_h, w, h, &x, &y);

	return window_pos_drag_from(pos, x, y, view_w, view_h, w, h, strip_w, strip_h, mx, my,
	                            clicked, down);
}
