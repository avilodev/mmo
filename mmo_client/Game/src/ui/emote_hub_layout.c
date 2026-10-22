// Where everything in the emote hub sits, in the menus' layout units.

#include "ui/emote_hub_layout.h"
#include "player/emote_clips.h"

#define HEAD_H 90.0f
#define PAD 30.0f
#define SLOT_LABEL_H 34.0f
#define SLOT_PITCH 188.0f
#define GRID_X 290.0f
#define GRID_Y 100.0f
#define GRID_GAP 16.0f
#define BAR_X 1624.0f
#define BAR_W 22.0f
#define CLOSE_SIZE 56.0f

int hub_rect_has(const hub_rect* r, float x, float y) {
	return x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h;
}

hub_rect hub_panel(float frame_w, float frame_h) {
	hub_rect r = { (frame_w - HUB_PANEL_W) * 0.5f, (frame_h - HUB_PANEL_H) * 0.5f,
	               HUB_PANEL_W, HUB_PANEL_H };

	return r;
}

hub_rect hub_close_button(const hub_rect* panel) {
	hub_rect r = { panel->x + panel->w - CLOSE_SIZE - 20.0f, panel->y + 17.0f, CLOSE_SIZE,
	               CLOSE_SIZE };

	return r;
}

hub_rect hub_slot_area(const hub_rect* panel, int slot) {
	hub_rect r = { panel->x + PAD, panel->y + HEAD_H + SLOT_PITCH * (float)slot,
	               HUB_SLOT_W, SLOT_LABEL_H + HUB_SLOT_H };

	return r;
}

hub_rect hub_slot_box(const hub_rect* panel, int slot) {
	hub_rect r = hub_slot_area(panel, slot);

	r.y += SLOT_LABEL_H;
	r.h = HUB_SLOT_H;

	return r;
}

hub_rect hub_cell_box(const hub_rect* panel, int place) {
	hub_rect r = { panel->x + GRID_X + (float)(place % HUB_COLS) * (HUB_CELL_W + GRID_GAP),
	               panel->y + GRID_Y + (float)(place / HUB_COLS) * (HUB_CELL_H + GRID_GAP),
	               HUB_CELL_W, HUB_CELL_H };

	return r;
}

hub_rect hub_scroll_track(const hub_rect* panel) {
	hub_rect r = { panel->x + BAR_X, panel->y + GRID_Y, BAR_W,
	               (float)HUB_ROWS * (HUB_CELL_H + GRID_GAP) - GRID_GAP };

	return r;
}

int hub_max_scroll(void) {
	int rows = (emote_hub_count() + HUB_COLS - 1) / HUB_COLS;

	return rows > HUB_ROWS ? rows - HUB_ROWS : 0;
}

int hub_clip_at(const hub_rect* panel, int scroll, float x, float y, int* out_place) {
	for(int place = 0; place < HUB_VISIBLE; place++) {
		hub_rect box = hub_cell_box(panel, place);
		int entry = scroll * HUB_COLS + place;

		if(!hub_rect_has(&box, x, y))
			continue;
		if(entry >= emote_hub_count())
			return -1;
		if(out_place)
			*out_place = place;

		return emote_hub_clip(entry);
	}

	return -1;
}

int hub_slot_at(const hub_rect* panel, float x, float y) {
	for(int slot = 0; slot < EMOTE_SLOT_COUNT; slot++) {
		hub_rect area = hub_slot_area(panel, slot);

		if(hub_rect_has(&area, x, y))
			return slot;
	}

	return -1;
}

int hub_scroll_for(const hub_rect* panel, float y) {
	hub_rect track = hub_scroll_track(panel);
	int max = hub_max_scroll();
	float t = (y - track.y) / track.h;
	int row = (int)(t * (float)(max + 1));

	if(row < 0)
		return 0;

	return row > max ? max : row;
}
