#ifndef EMOTE_HUB_LAYOUT_H
#define EMOTE_HUB_LAYOUT_H

// Where everything in the emote hub sits, in the menus' layout units. No GL.

#include "player/emote_slots.h"
#include "protocol.h"

#define HUB_PANEL_W 1700.0f
#define HUB_PANEL_H 1260.0f

// The grid of every clip: boxes across and down, shown at once
#define HUB_COLS 5
#define HUB_ROWS 5
#define HUB_VISIBLE (HUB_COLS * HUB_ROWS)

// Each box in the grid, and a slot's box on the left
#define HUB_CELL_W 250.0f
#define HUB_CELL_H 200.0f
#define HUB_SLOT_W 180.0f
#define HUB_SLOT_H 144.0f

typedef struct {
	float x;
	float y;
	float w;
	float h;
} hub_rect;

int hub_rect_has(const hub_rect* r, float x, float y);

// The window, centred in a frame of the given size
hub_rect hub_panel(float frame_w, float frame_h);

hub_rect hub_close_button(const hub_rect* panel);

// A slot's box, and the box with its name above it: what a clip is dropped on
hub_rect hub_slot_box(const hub_rect* panel, int slot);
hub_rect hub_slot_area(const hub_rect* panel, int slot);

// The grid's box at a place among the visible ones, 0 to HUB_VISIBLE - 1
hub_rect hub_cell_box(const hub_rect* panel, int place);

// The scrollbar's track
hub_rect hub_scroll_track(const hub_rect* panel);

// How far the grid can scroll, in rows
int hub_max_scroll(void);

// The clip under a point when the grid is scrolled a number of rows, or -1.
// out_place gets its place among the visible boxes when it is not NULL.
int hub_clip_at(const hub_rect* panel, int scroll, float x, float y, int* out_place);

// The slot whose box or name is under a point, or -1
int hub_slot_at(const hub_rect* panel, float x, float y);

// The row the grid should be scrolled to for a point on the track
int hub_scroll_for(const hub_rect* panel, float y);

#endif /* EMOTE_HUB_LAYOUT_H */
