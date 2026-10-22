/**
 * @file
 * Draw the character select scene and read its input.
 */
#include "ui/char_select_view.h"
#include "ui/char_select_nav.h"
#include "ui/char_select_preview.h"
#include "ui/ui_kit.h"
#include "input/input.h"
#include "renderer.h"

#include <stdio.h>
#include <string.h>
#include <winsock2.h>

#define MARGIN         60.0f
#define LEFT_W         640.0f
#define NEW_PANEL_H    200.0f   // the left panel's height for a new character
#define NO_PASSIVE_PANEL_H 410.0f // for a race with no passive
#define FIELDS_PANEL_H 568.0f   // for one line of passive text; each more adds a line
#define SELECT_W       600.0f
#define SELECT_ARROW   64.0f
#define ENTRY_H        56.0f
#define ENTRY_GAP      6.0f
#define BUTTON_H       72.0f
#define BACK_W         220.0f
#define DELETE_W       220.0f
#define JOIN_W         440.0f
#define JOIN_H         88.0f
#define CONFIRM_W      280.0f
#define CONFIRM_H      66.0f
#define PICTURE_FRAC   0.78f    // the picture's height, of the screen's
#define PICTURE_FLOOR  15.0f    // layout units between its bottom and the screen's
#define NAME_FROM_BOT  110.0f   // the name's middle, up from the screen's bottom
#define DEFAULT_YAW    0.35f    // turned a little from the viewer, not flat on
#define TURN_PER_PIXEL 0.012f   // radians of turn for each pixel of middle-drag
#define WRAP_LINES     5
#define WRAP_LINE_LEN  96

static const kit_color ERROR_RED = { 1.00f, 0.45f, 0.40f };

static int s_confirm;       // the delete prompt is up
static int s_confirm_focus; // 0 cancels, 1 deletes
static float s_yaw = DEFAULT_YAW; // the character's turn, in radians; 0 faces the viewer

typedef struct {
	kit_frame frame;
	float mx;           // the pointer, in layout units
	float my;
} view_ctx;

static view_ctx make_ctx(const GameState* game) {
	view_ctx c;

	c.frame = ui_kit_frame(game->camera.viewport_width, game->camera.viewport_height,
	                       game->settings.ui_scale);
	ui_kit_to_layout(&c.frame, game->input.mouse_x, game->input.mouse_y, &c.mx, &c.my);

	return c;
}

// Characters in the list, kept inside what the packet can hold
static int character_count(const GameState* game) {
	int n = game->char_select.list.count;

	if(n < 0)
		return 0;

	return n > 10 ? 10 : n;
}

static float select_x(const view_ctx* c) {
	return c->frame.w - MARGIN - SELECT_W;
}

static float entry_y(int n) {
	return MARGIN + SELECT_ARROW + 16.0f + (ENTRY_H + ENTRY_GAP) * (float)n;
}

static float arrow_x(const view_ctx* c, int right) {
	return right ? c->frame.w - MARGIN - SELECT_ARROW : select_x(c);
}

static float join_x(const view_ctx* c) {
	return c->frame.w - MARGIN - JOIN_W;
}

static float join_y(const view_ctx* c) {
	return c->frame.h - MARGIN - JOIN_H;
}

static float bottom_y(const view_ctx* c) {
	return c->frame.h - MARGIN - BUTTON_H;
}

static float delete_x(void) {
	return MARGIN + BACK_W + 20.0f;
}

static float confirm_x(const view_ctx* c, int n) {
	return c->frame.w * 0.5f - CONFIRM_W - 7.0f + (CONFIRM_W + 14.0f) * (float)n;
}

static float confirm_y(const view_ctx* c) {
	return c->frame.h * 0.5f + 40.0f;
}

static const RaceInfo* find_race(const GameState* game, uint32_t race_id) {
	const RaceListResponsePacket* races = &game->char_select.races;

	if(!game->char_select.races_loaded)
		return NULL;

	for(int i = 0; i < races->count; i++) {
		if(ntohl(races->races[i].race_id) == race_id)
			return &races->races[i];
	}

	return NULL;
}

static const char* role_name(uint8_t role) {
	switch(role) {
	case ROLE_TANK:
		return "Tank";
	case ROLE_DPS:
		return "DPS";
	case ROLE_HEALER:
		return "Healer";
	default:
		return "Unknown";
	}
}

// Whether entry n is a character, and not the new character entry
static int is_character(const GameState* game, int n) {
	return n >= 0 && n < character_count(game);
}

void csv_open(void) {
	ui_kit_load();
	s_confirm = 0;
	s_confirm_focus = 0;
	s_yaw = DEFAULT_YAW;
}

void csv_close(void) {
	s_confirm = 0;
	char_select_preview_release();
}

void csv_render_preview(const GameState* game, float dt) {
	int count = character_count(game);
	int at = cs_nav_clamp(game->char_select.selected_index, count);
	const RaceInfo* race = NULL;
	uint32_t appearance = 0;

	if(!game->char_select.loaded || !is_character(game, at))
		return;

	race = find_race(game, ntohl(game->char_select.list.characters[at].race_id));
	appearance = ntohl(game->char_select.list.characters[at].appearance);
	char_select_preview_render(race ? race->key : NULL, appearance, s_yaw, dt);
}

int csv_escape(void) {
	if(!s_confirm)
		return 0;

	s_confirm = 0;

	return 1;
}

// Moves the selection and drops an old error that belonged to the last one
static void select_entry(GameState* game, int index) {
	if(index == game->char_select.selected_index)
		return;

	game->char_select.selected_index = index;
	s_yaw = DEFAULT_YAW;
	memset(game->char_select.error_message, 0, sizeof(game->char_select.error_message));
}

static csv_action update_confirm(const view_ctx* c, const InputState* in) {
	int pick = -1;

	if(input_key_just_pressed(in, GLFW_KEY_LEFT))
		s_confirm_focus = 0;

	if(input_key_just_pressed(in, GLFW_KEY_RIGHT))
		s_confirm_focus = 1;

	if(input_key_just_pressed(in, GLFW_KEY_ENTER))
		pick = s_confirm_focus;

	for(int n = 0; n < 2; n++) {
		if(!ui_kit_hit(c->mx, c->my, confirm_x(c, n), confirm_y(c), CONFIRM_W, CONFIRM_H))
			continue;

		if(in->mouse_dx != 0.0f || in->mouse_dy != 0.0f)
			s_confirm_focus = n;

		if(in->mouse_left_clicked)
			pick = n;
	}

	if(pick < 0)
		return CSV_NONE;

	s_confirm = 0;

	return pick == 1 ? CSV_DELETE : CSV_NONE;
}

// The arrows and the name list in the top right
static void update_select(GameState* game, const view_ctx* c, int count, int at) {
	const InputState* in = &game->input;
	int next = at;

	if(input_key_just_pressed(in, GLFW_KEY_LEFT))
		next = cs_nav_step(at, count, -1);

	if(input_key_just_pressed(in, GLFW_KEY_RIGHT))
		next = cs_nav_step(at, count, 1);

	if(in->mouse_left_clicked) {
		for(int side = 0; side < 2; side++) {
			if(ui_kit_hit(c->mx, c->my, arrow_x(c, side), MARGIN, SELECT_ARROW, SELECT_ARROW))
				next = cs_nav_step(at, count, side ? 1 : -1);
		}

		for(int n = 0; n < cs_nav_count(count); n++) {
			if(ui_kit_hit(c->mx, c->my, select_x(c), entry_y(n), SELECT_W, ENTRY_H))
				next = n;
		}
	}

	select_entry(game, next);
}

csv_action csv_update(GameState* game, int busy) {
	const InputState* in = &game->input;
	view_ctx c = make_ctx(game);
	int count = character_count(game);
	int at = cs_nav_clamp(game->char_select.selected_index, count);
	int is_new = cs_nav_is_new(at, count);

	// Holding the middle button and moving the pointer turns the character
	if(in->mouse_middle_down)
		s_yaw += in->mouse_dx * TURN_PER_PIXEL;

	if(in->mouse_left_clicked &&
	   ui_kit_hit(c.mx, c.my, MARGIN, bottom_y(&c), BACK_W, BUTTON_H))
		return CSV_BACK;

	if(!game->char_select.loaded || busy)
		return CSV_NONE;

	if(s_confirm)
		return update_confirm(&c, in);

	update_select(game, &c, count, at);
	at = cs_nav_clamp(game->char_select.selected_index, count);
	is_new = cs_nav_is_new(at, count);

	if(input_key_just_pressed(in, GLFW_KEY_ENTER))
		return is_new ? CSV_NEW : CSV_JOIN;

	if(!in->mouse_left_clicked)
		return CSV_NONE;

	if(ui_kit_hit(c.mx, c.my, join_x(&c), join_y(&c), JOIN_W, JOIN_H))
		return is_new ? CSV_NEW : CSV_JOIN;

	if(!is_new && ui_kit_hit(c.mx, c.my, delete_x(), bottom_y(&c), DELETE_W, BUTTON_H)) {
		s_confirm = 1;
		s_confirm_focus = 0;
	}

	return CSV_NONE;
}

// Breaks text into lines no wider than a width; the last is cut short with
// dots when the text does not fit. Returns the number of lines.
static int wrap_text(const char* text, float size, float width,
		char lines[][WRAP_LINE_LEN]) {
	int n = 0;
	char word[WRAP_LINE_LEN];
	char trial[WRAP_LINE_LEN * 2];

	lines[0][0] = '\0';

	while(*text) {
		size_t len = 0;

		while(*text == ' ')
			text++;

		while(*text && *text != ' ' && len < sizeof(word) - 1)
			word[len++] = *text++;

		word[len] = '\0';
		if(len == 0)
			break;

		snprintf(trial, sizeof(trial), "%s%s%s", lines[n], lines[n][0] ? " " : "", word);

		if(lines[n][0] && ui_kit_text_width(trial, size) > width) {
			if(n == WRAP_LINES - 1) {
				size_t keep = strlen(lines[n]);

				if(keep > 3)
					keep -= 3;

				snprintf(lines[n] + keep, WRAP_LINE_LEN - keep, "...");
				return n + 1;
			}

			n++;
			snprintf(lines[n], WRAP_LINE_LEN, "%s", word);
		} else {
			snprintf(lines[n], WRAP_LINE_LEN, "%.*s", WRAP_LINE_LEN - 1, trial);
		}
	}

	return lines[n][0] ? n + 1 : n;
}

// A small heading with its value under it; returns the y after them
static float draw_field(float x, float y, const char* label, const char* value,
		kit_color color) {
	ui_kit_text(x, y, label, 22.0f, KIT_HEAD, KIT_LEFT);
	ui_kit_text(x, y + 46.0f, value, 40.0f, color, KIT_LEFT);

	return y + 118.0f;
}

static void draw_class_panel(const GameState* game, int at) {
	float x = MARGIN;
	float y = MARGIN;
	float tx = x + 44.0f;
	float ty = y + 56.0f;
	float h = NEW_PANEL_H;
	const RaceInfo* race = NULL;
	char line[96];
	char lines[WRAP_LINES][WRAP_LINE_LEN];
	int rows = -1; // -1 when the race has no passive to show

	if(!is_character(game, at)) {
		ui_kit_panel(x, y, LEFT_W, h);
		ui_kit_text(tx, ty, "New Character", 40.0f, KIT_GOLD, KIT_LEFT);
		ui_kit_text(tx, ty + 70.0f, "Choose this to begin a", 28.0f, KIT_DIM, KIT_LEFT);
		ui_kit_text(tx, ty + 108.0f, "new character on this world.", 28.0f, KIT_DIM, KIT_LEFT);

		return;
	}

	race = find_race(game, ntohl(game->char_select.list.characters[at].race_id));

	if(race && race->passive_name[0]) {
		rows = wrap_text(race->passive_desc, 26.0f, LEFT_W - 88.0f, lines);
		h = FIELDS_PANEL_H + 36.0f * (float)(rows > 1 ? rows - 1 : 0);
	} else {
		h = NO_PASSIVE_PANEL_H;
	}

	ui_kit_panel(x, y, LEFT_W, h);
	snprintf(line, sizeof(line), "%s", race ? race->name : "Unknown");
	ty = draw_field(tx, ty, "RACE", line, KIT_WHITE);

	if(race)
		ui_kit_text(tx, ty - 30.0f, race->latin, 26.0f, KIT_DIM, KIT_LEFT);

	snprintf(line, sizeof(line), "%lu", (unsigned long)ntohl(game->char_select.list.characters[at].level));
	ty = draw_field(tx, ty + 26.0f, "LEVEL", line, KIT_WHITE);
	ty = draw_field(tx, ty, "ROLE", race ? role_name(race->default_role) : "Unknown",
	                KIT_WHITE);

	if(rows < 0)
		return;

	ui_kit_text(tx, ty, "PASSIVE", 22.0f, KIT_HEAD, KIT_LEFT);
	ui_kit_text(tx, ty + 44.0f, race->passive_name, 34.0f, KIT_GOLD, KIT_LEFT);

	for(int i = 0; i < rows; i++)
		ui_kit_text(tx, ty + 88.0f + 36.0f * (float)i, lines[i], 26.0f, KIT_DIM, KIT_LEFT);
}

static kit_btn_state button_state(const view_ctx* c, const InputState* in, float x,
		float y, float w, float h) {
	if(!ui_kit_hit(c->mx, c->my, x, y, w, h))
		return KIT_BTN_NORMAL;

	return in->mouse_left_down ? KIT_BTN_DOWN : KIT_BTN_HOVER;
}

static void draw_selector(const GameState* game, const view_ctx* c, int count, int at) {
	const InputState* in = &game->input;
	float x = select_x(c);
	char line[64];

	for(int side = 0; side < 2; side++) {
		float ax = arrow_x(c, side);

		ui_kit_button(ax, MARGIN, SELECT_ARROW, SELECT_ARROW, side ? ">" : "<", 0,
		              button_state(c, in, ax, MARGIN, SELECT_ARROW, SELECT_ARROW));
	}

	if(is_character(game, at))
		snprintf(line, sizeof(line), "%s", game->char_select.list.characters[at].name);
	else
		snprintf(line, sizeof(line), "New Character");

	ui_kit_text(x + SELECT_W * 0.5f, MARGIN + SELECT_ARROW * 0.5f, line, 36.0f, KIT_GOLD,
	            KIT_CENTER);

	for(int n = 0; n < cs_nav_count(count); n++) {
		float y = entry_y(n);
		int hovered = ui_kit_hit(c->mx, c->my, x, y, SELECT_W, ENTRY_H);

		if(n == at)
			renderer_draw_rect(x, y, SELECT_W, ENTRY_H, 1.0f, 0.80f, 0.47f, 0.28f);
		else if(hovered)
			renderer_draw_rect(x, y, SELECT_W, ENTRY_H, 1.0f, 1.0f, 1.0f, 0.12f);
		else
			renderer_draw_rect(x, y, SELECT_W, ENTRY_H, 0.0f, 0.0f, 0.0f, 0.35f);

		if(is_character(game, n)) {
			ui_kit_text(x + 20.0f, y + ENTRY_H * 0.5f,
			            game->char_select.list.characters[n].name, 28.0f, KIT_WHITE, KIT_LEFT);
			snprintf(line, sizeof(line), "Lv. %lu",
			         (unsigned long)ntohl(game->char_select.list.characters[n].level));
			ui_kit_text(x + SELECT_W - 20.0f, y + ENTRY_H * 0.5f, line, 26.0f, KIT_DIM,
			            KIT_RIGHT);
		} else {
			ui_kit_text(x + 20.0f, y + ENTRY_H * 0.5f, "+ New Character", 28.0f, KIT_GOLD,
			            KIT_LEFT);
		}
	}
}

// A dark backdrop; the middle is lit when there is a character to stand in it
static void draw_backdrop(const view_ctx* c, int lit) {
	float cx = c->frame.w * 0.5f;

	renderer_draw_rect(0.0f, 0.0f, c->frame.w, c->frame.h, 0.03f, 0.05f, 0.09f, 1.0f);

	if(!lit)
		return;

	for(int i = 0; i < 8; i++) {
		float w = 1800.0f - 200.0f * (float)i;

		renderer_draw_rect(cx - w * 0.5f, 0.0f, w, c->frame.h, 0.10f, 0.16f, 0.28f, 0.07f);
	}
}

static void draw_picture(const view_ctx* c) {
	unsigned int texture = char_select_preview_texture();
	float h = c->frame.h * PICTURE_FRAC;
	float w = h * (float)CS_PREVIEW_PX_W / (float)CS_PREVIEW_PX_H;

	if(!texture)
		return;

	renderer_draw_sprite_uv((c->frame.w - w) * 0.5f, c->frame.h - PICTURE_FLOOR - h, w, h,
	                        texture, 0.0f, 1.0f, 1.0f, 0.0f);
}

static void draw_confirm(const GameState* game, const view_ctx* c, int at) {
	float w = 760.0f;
	float h = 300.0f;
	float x = (c->frame.w - w) * 0.5f;
	float y = (c->frame.h - h) * 0.5f - 20.0f;
	char line[64];

	ui_kit_dim(0.0f, 0.0f, c->frame.w, c->frame.h, 0.72f);
	renderer_draw_rect(x, y, w, h, 0.05f, 0.05f, 0.09f, 0.97f);
	ui_kit_panel(x, y, w, h);

	if(is_character(game, at))
		snprintf(line, sizeof(line), "Delete %s?", game->char_select.list.characters[at].name);
	else
		snprintf(line, sizeof(line), "Delete character?");

	ui_kit_text(c->frame.w * 0.5f, y + 70.0f, line, 34.0f, KIT_GOLD, KIT_CENTER);
	ui_kit_text(c->frame.w * 0.5f, y + 130.0f, "This cannot be undone.", 26.0f, KIT_DIM,
	            KIT_CENTER);

	for(int n = 0; n < 2; n++) {
		kit_btn_state state = button_state(c, &game->input, confirm_x(c, n), confirm_y(c),
		                                   CONFIRM_W, CONFIRM_H);

		ui_kit_button(confirm_x(c, n), confirm_y(c), CONFIRM_W, CONFIRM_H,
		              n == 0 ? "Cancel" : "Delete", n == s_confirm_focus, state);
	}
}

void csv_draw(const GameState* game, const char* status) {
	view_ctx c = make_ctx(game);
	const InputState* in = &game->input;
	int count = character_count(game);
	int at = cs_nav_clamp(game->char_select.selected_index, count);
	int is_new = cs_nav_is_new(at, count);
	int loaded = game->char_select.loaded;

	ui_kit_begin(&c.frame);
	draw_backdrop(&c, loaded && !is_new);

	ui_kit_button(MARGIN, bottom_y(&c), BACK_W, BUTTON_H, "Back", 0,
	              button_state(&c, in, MARGIN, bottom_y(&c), BACK_W, BUTTON_H));

	if(!loaded) {
		ui_kit_text(c.frame.w * 0.5f, c.frame.h * 0.5f, "Loading characters...", 40.0f,
		            KIT_DIM, KIT_CENTER);
		ui_kit_end();

		return;
	}

	if(!is_new) {
		draw_picture(&c);
		ui_kit_text(c.frame.w * 0.5f, c.frame.h - NAME_FROM_BOT,
		            game->char_select.list.characters[at].name, 60.0f, KIT_WHITE, KIT_CENTER);
		ui_kit_button(delete_x(), bottom_y(&c), DELETE_W, BUTTON_H, "Delete", 0,
		              button_state(&c, in, delete_x(), bottom_y(&c), DELETE_W, BUTTON_H));
	}

	draw_class_panel(game, at);
	draw_selector(game, &c, count, at);
	ui_kit_button(join_x(&c), join_y(&c), JOIN_W, JOIN_H, is_new ? "New Character" : "Join World",
	              1, button_state(&c, in, join_x(&c), join_y(&c), JOIN_W, JOIN_H));

	if(game->char_select.error_message[0])
		ui_kit_text(c.frame.w * 0.5f, c.frame.h - NAME_FROM_BOT - 70.0f,
		            game->char_select.error_message, 28.0f, ERROR_RED, KIT_CENTER);

	if(status) {
		ui_kit_dim(0.0f, c.frame.h * 0.5f - 50.0f, c.frame.w, 100.0f, 0.55f);
		ui_kit_text(c.frame.w * 0.5f, c.frame.h * 0.5f, status, 40.0f, KIT_GOLD, KIT_CENTER);
	}

	if(s_confirm)
		draw_confirm(game, &c, at);

	ui_kit_end();
}
