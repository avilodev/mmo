#ifndef WEATHER_LAYOUT_H
#define WEATHER_LAYOUT_H

// Where everything in the weather window sits, in layout units. No GL.

#include "protocol.h"

#define WEATHER_PANEL_W 700.0f
#define WEATHER_PANEL_H 880.0f
#define WEATHER_DRAWER_W 760.0f

// The weather window draws this much smaller than the other menus
#define WEATHER_FRAME_SHRINK 0.75f

// The player's pointer on the weather map, in the window's layout units. The full
// map sizes its pointer from this too, so the two always match on screen.
#define WEATHER_POINTER_UNITS 6.0f

typedef struct {
	float x;
	float y;
	float w;
	float h;
} weather_rect;

int weather_rect_has(const weather_rect* r, float x, float y);

// The window, centred in a frame of the given size
weather_rect weather_panel(float frame_w, float frame_h);

weather_rect weather_close_button(const weather_rect* panel);

// The line that names the place the forecast is for
weather_rect weather_place_block(const weather_rect* panel);

// The block that says what the weather is right now
weather_rect weather_now_block(const weather_rect* panel);

// The row of a day ahead (0 is today), and the parts of a row
weather_rect weather_day_row(const weather_rect* panel, int ahead);
weather_rect weather_row_icon(const weather_rect* row);
weather_rect weather_row_precip(const weather_rect* row);
weather_rect weather_row_lo(const weather_rect* row);
weather_rect weather_row_bar(const weather_rect* row);
weather_rect weather_row_hi(const weather_rect* row);

// The arrow on the right edge that opens the weather map; it stays on the seam
// between the window and the drawer, open or not
weather_rect weather_arrow_button(const weather_rect* panel);

// The drawer that slides out of the window's right edge, as tall as the window
weather_rect weather_drawer_panel(const weather_rect* panel);

// Where the map sits inside the drawer
weather_rect weather_drawer_inner(const weather_rect* drawer);

// The small Reset button, bottom right on the map, that brings the view back to the player
weather_rect weather_drawer_recenter_button(const weather_rect* inner);

// Where the line about the next update goes
weather_rect weather_footer(const weather_rect* panel);

// The part of a range bar a day's low to high covers on the given scale
void weather_bar_span(const weather_rect* bar, int scale_lo, int scale_hi, int lo, int hi,
		float* out_x, float* out_w);

#endif /* WEATHER_LAYOUT_H */
