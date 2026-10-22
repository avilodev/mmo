/**
 * @file
 * Run the emote hub window: input, the boxes' pictures, and drawing.
 */

#include "ui/emote_hub.h"
#include "ui/emote_hub_layout.h"
#include "ui/emote_preview.h"
#include "ui/ui_kit.h"
#include "core/keybinds.h"
#include "core/race_registry.h"
#include "input/input.h"
#include "player/emote_clips.h"
#include "player/emote_slots.h"
#include "renderer.h"

#include <GLFW/glfw3.h>
#include <stdio.h>
#include <string.h>

#define LABEL_SIZE 30.0f
#define BOX_LABEL_SIZE 24.0f
#define SLOT_LABEL_SIZE 22.0f
#define STRIP_H 34.0f
#define SLOT_STRIP_H 30.0f
#define GHOST_SCALE 0.5f

// The first picture cell the grid uses; the slots have the cells before it
#define GRID_CELL_BASE EMOTE_SLOT_COUNT

// Grid bodies start here, so a clip keeps its own body as it scrolls
#define GRID_BODY_BASE 100

static int s_open;
static int s_scroll; // the first visible row of the grid
static int s_down; // the left button was held at the last update
static int s_drag_clip = -1; // the clip being carried, or -1
static int s_drag_cell = -1; // the picture the carried clip shows
static int s_bar_drag; // the scrollbar is being dragged

int emote_hub_is_open(void) {
	return s_open;
}

void emote_hub_open(void) {
	s_open = 1;
	s_drag_clip = -1;
	s_bar_drag = 0;
}

void emote_hub_close(void) {
	s_open = 0;
	s_drag_clip = -1;
	s_bar_drag = 0;
}

void emote_hub_reset(void) {
	emote_hub_close();
	s_scroll = 0;
	s_down = 0;
	emote_preview_release();
}

int emote_hub_scroll(float wheel) {
	int max = hub_max_scroll();

	if(!s_open || wheel == 0.0f)
		return 0;

	s_scroll += wheel > 0.0f ? -1 : 1;
	if(s_scroll < 0)
		s_scroll = 0;
	if(s_scroll > max)
		s_scroll = max;

	return 1;
}

// The game must not see the keys and clicks the window used
static void swallow_input(InputState* in) {
	in->mouse_left_clicked = 0;
	in->mouse_left_down = 0;
	in->mouse_right_clicked = 0;
	in->mouse_right_down = 0;
	in->mouse_middle_down = 0;
	memset(in->mouse_buttons_down, 0, sizeof(in->mouse_buttons_down));
	memset(in->mouse_buttons_pressed, 0, sizeof(in->mouse_buttons_pressed));
	memset(in->keys_pressed, 0, sizeof(in->keys_pressed));
	memset(in->keys_just_pressed, 0, sizeof(in->keys_just_pressed));
}

// A press on the grid picks a clip up; the scrollbar follows the pointer
static void press(const hub_rect* panel, float mx, float my) {
	hub_rect track = hub_scroll_track(panel);
	int place = 0;
	int clip = hub_clip_at(panel, s_scroll, mx, my, &place);

	if(clip >= 0) {
		s_drag_clip = clip;
		s_drag_cell = GRID_CELL_BASE + place;
	} else if(hub_rect_has(&track, mx, my)) {
		s_bar_drag = 1;
	}
}

// Letting go over a slot puts the carried clip there
static void release(const hub_rect* panel, float mx, float my) {
	int slot = hub_slot_at(panel, mx, my);

	if(s_drag_clip >= 0 && slot >= 0)
		emote_slots_set(slot, s_drag_clip);

	s_drag_clip = -1;
	s_bar_drag = 0;
}

void emote_hub_update(GameState* game) {
	InputState* in = &game->input;
	kit_frame frame = ui_kit_frame(game->camera.viewport_width, game->camera.viewport_height,
	                               game->settings.ui_scale);
	hub_rect panel = hub_panel(frame.w, frame.h);
	hub_rect close = hub_close_button(&panel);
	float mx;
	float my;
	int down = in->mouse_left_down;

	ui_kit_to_layout(&frame, in->mouse_x, in->mouse_y, &mx, &my);

	if(input_key_just_pressed(in, GLFW_KEY_ESCAPE) ||
	   input_key_just_pressed(in, g_keybinds.toggle_emote_hub) ||
	   (in->mouse_left_clicked && hub_rect_has(&close, mx, my))) {
		emote_hub_close();
		swallow_input(in);
		s_down = 0;

		return;
	}

	if(in->mouse_left_clicked)
		press(&panel, mx, my);
	if(s_bar_drag && down)
		s_scroll = hub_scroll_for(&panel, my);
	if(s_down && !down)
		release(&panel, mx, my);

	s_down = down;
	swallow_input(in);
}

// The race and look the boxes dress the body in
static void player_look(const GameState* game, const char** race_key, uint32_t* appearance) {
	const RaceInfo* race = NULL;

	*race_key = NULL;
	*appearance = 0;
	if(!game->player.info_loaded)
		return;

	race = client_race_find(game->player.info.race_id);
	*race_key = race ? race->key : NULL;
	*appearance = game->player.info.appearance;
}

static preview_kind kind_of_slot(int slot) {
	if(slot == EMOTE_SLOT_WALK)
		return PREVIEW_WALK;
	if(slot == EMOTE_SLOT_RUN)
		return PREVIEW_RUN;

	return slot == EMOTE_SLOT_SIT ? PREVIEW_SIT : PREVIEW_EMOTE;
}

// Every box on screen: the six slots, then the visible part of the grid
static int collect_jobs(preview_job* jobs) {
	int n = 0;

	for(int slot = 0; slot < EMOTE_SLOT_COUNT; slot++) {
		jobs[n].cell = slot;
		jobs[n].body = (unsigned int)slot;
		jobs[n].kind = kind_of_slot(slot);
		jobs[n].clip = emote_slots_get(slot);
		n++;
	}

	for(int place = 0; place < HUB_VISIBLE; place++) {
		int clip = emote_hub_clip(s_scroll * HUB_COLS + place);

		if(clip < 0)
			break;

		jobs[n].cell = GRID_CELL_BASE + place;
		jobs[n].body = GRID_BODY_BASE + (unsigned int)clip;
		jobs[n].kind = PREVIEW_EMOTE;
		jobs[n].clip = clip;
		n++;
	}

	return n;
}

void emote_hub_render_previews(const GameState* game) {
	preview_job jobs[EMOTE_PREVIEW_CELLS];
	const char* race_key;
	uint32_t appearance;
	int n;

	if(!s_open)
		return;

	player_look(game, &race_key, &appearance);
	n = collect_jobs(jobs);
	emote_preview_render(jobs, n, race_key, appearance, game->playing->frame_dt);
}

// The text cut short with dots until it fits a width
static void fit_text(const char* text, float size, float width, char* out, size_t out_size) {
	size_t n;

	snprintf(out, out_size, "%s", text);
	n = strlen(out);

	while(n > 3 && ui_kit_text_width(out, size) > width) {
		out[n - 1] = '\0';
		n--;
		if(n > 3)
			memcpy(out + n - 3, "...", 3);
	}
}

// A picture with its name along the bottom; gold when `lit`
static void draw_box(const hub_rect* r, unsigned int texture, const char* label,
		float text_size, float strip_h, int lit) {
	char fitted[64];

	if(lit)
		renderer_draw_rect(r->x - 4.0f, r->y - 4.0f, r->w + 8.0f, r->h + 8.0f, 1.0f, 0.80f,
		                   0.47f, 1.0f);
	else
		renderer_draw_rect(r->x - 3.0f, r->y - 3.0f, r->w + 6.0f, r->h + 6.0f, 0.30f, 0.33f,
		                   0.45f, 1.0f);

	if(texture)
		renderer_draw_sprite_uv(r->x, r->y, r->w, r->h, texture, 0.0f, 1.0f, 1.0f, 0.0f);
	else
		renderer_draw_rect(r->x, r->y, r->w, r->h, 0.10f, 0.11f, 0.16f, 1.0f);

	renderer_draw_rect(r->x, r->y + r->h - strip_h, r->w, strip_h, 0.0f, 0.0f, 0.0f, 0.62f);
	fit_text(label, text_size, r->w - 16.0f, fitted, sizeof(fitted));
	ui_kit_text(r->x + r->w * 0.5f, r->y + r->h - strip_h * 0.5f, fitted, text_size, KIT_WHITE,
	            KIT_CENTER);
}

static void draw_slot(const hub_rect* panel, int slot, float mx, float my) {
	hub_rect area = hub_slot_area(panel, slot);
	hub_rect box = hub_slot_box(panel, slot);
	char name[48];
	char key[24] = "";
	const char* key_name = keybinds_key_name(emote_slot_key(slot));

	if(key_name)
		snprintf(key, sizeof(key), "[%s]", key_name);

	ui_kit_text(area.x, area.y + 15.0f, emote_slot_label(slot), LABEL_SIZE, KIT_GOLD, KIT_LEFT);
	ui_kit_text(area.x + area.w, area.y + 15.0f, key, SLOT_LABEL_SIZE, KIT_DIM, KIT_RIGHT);

	emote_clip_label(emote_slots_get(slot), name, sizeof(name));
	draw_box(&box, emote_preview_texture(slot), name, SLOT_LABEL_SIZE, SLOT_STRIP_H,
	         s_drag_clip >= 0 && hub_rect_has(&area, mx, my));
}

static void draw_grid(const hub_rect* panel, float mx, float my) {
	char name[48];

	for(int place = 0; place < HUB_VISIBLE; place++) {
		hub_rect box = hub_cell_box(panel, place);
		int clip = emote_hub_clip(s_scroll * HUB_COLS + place);

		if(clip < 0)
			break;

		emote_clip_label(clip, name, sizeof(name));
		draw_box(&box, emote_preview_texture(GRID_CELL_BASE + place), name, BOX_LABEL_SIZE,
		         STRIP_H, s_drag_clip < 0 && hub_rect_has(&box, mx, my));
	}
}

static void draw_scrollbar(const hub_rect* panel) {
	hub_rect track = hub_scroll_track(panel);
	int max = hub_max_scroll();
	float thumb_h = track.h / (float)(max + 1);
	float thumb_y = track.y + (track.h - thumb_h) * (max ? (float)s_scroll / (float)max : 0.0f);

	renderer_draw_rect(track.x, track.y, track.w, track.h, 0.0f, 0.0f, 0.0f, 0.45f);
	renderer_draw_rect(track.x, thumb_y, track.w, thumb_h, 0.55f, 0.65f, 0.78f, 0.95f);
}

// The carried clip's picture follows the pointer
static void draw_ghost(float mx, float my) {
	hub_rect r = { mx - HUB_CELL_W * GHOST_SCALE * 0.5f, my - HUB_CELL_H * GHOST_SCALE * 0.5f,
	               HUB_CELL_W * GHOST_SCALE, HUB_CELL_H * GHOST_SCALE };
	char name[48];

	if(s_drag_clip < 0)
		return;

	emote_clip_label(s_drag_clip, name, sizeof(name));
	draw_box(&r, emote_preview_texture(s_drag_cell), name, SLOT_LABEL_SIZE, SLOT_STRIP_H, 1);
}

void emote_hub_draw(const GameState* game) {
	kit_frame frame;
	hub_rect panel;
	hub_rect close;
	float mx;
	float my;

	if(!s_open)
		return;

	frame = ui_kit_frame(game->camera.viewport_width, game->camera.viewport_height,
	                     game->settings.ui_scale);
	panel = hub_panel(frame.w, frame.h);
	close = hub_close_button(&panel);
	ui_kit_to_layout(&frame, game->input.mouse_x, game->input.mouse_y, &mx, &my);

	ui_kit_begin(&frame);
	ui_kit_dim(0.0f, 0.0f, frame.w, frame.h, 0.55f);
	renderer_draw_rect(panel.x, panel.y, panel.w, panel.h, 0.05f, 0.05f, 0.09f, 0.97f);
	ui_kit_panel(panel.x, panel.y, panel.w, panel.h);
	ui_kit_text(panel.x + panel.w * 0.5f, panel.y + 45.0f, "Emotes", 40.0f, KIT_GOLD, KIT_CENTER);
	ui_kit_close_button(close.x, close.y, close.w, hub_rect_has(&close, mx, my));
	renderer_draw_rect(panel.x + 265.0f, panel.y + 90.0f, 2.0f, panel.h - 120.0f, 1.0f, 1.0f,
	                   1.0f, 0.15f);

	for(int slot = 0; slot < EMOTE_SLOT_COUNT; slot++)
		draw_slot(&panel, slot, mx, my);

	draw_grid(&panel, mx, my);
	draw_scrollbar(&panel);
	draw_ghost(mx, my);
	ui_kit_end();
}
