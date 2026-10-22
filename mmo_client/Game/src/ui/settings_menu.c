/**
 * @file
 * Draw the settings menu and apply its input.
 */
#include "ui/settings_menu.h"
#include "ui/keybinds_page.h"
#include "ui/ui_kit.h"
#include "core/keybinds.h"
#include "game.h"
#include "input/input.h"
#include "renderer.h"
#include <string.h>

#define SM_TAB_H       60.0f
#define SM_TAB_GAP     12.0f
#define SM_PANEL_TOP   130.0f
#define SM_FOOTER_H    190.0f
#define SM_ROW         74.0f
#define SM_FIRST_ROW   56.0f
#define SM_BTN_W       270.0f
#define SM_BTN_H       66.0f
#define SM_BTN_GAP     20.0f
#define SM_CTL_FRAC    0.465f
#define SM_CLOSE       60.0f

typedef struct {
	kit_frame frame;
	float tab_x;
	float tab_w;
	float px;
	float py;
	float pw;
	float ph;
	float label_x;
	float ctl_x;
	float ctl_w;
	float btn_x;     // left edge of the Back button
	float btn_y;
} layout;

static int s_open;
static sf_tab s_tab;
static GameSettings s_pending;
static int s_dropdown = -1;     // row of the open list, or -1
static int s_drag = -1;         // row of the slider being dragged, or -1
static int s_confirm;           // the discard prompt is up

static layout make_layout(const GameState* game) {
	layout l;
	float tabs_w;

	l.frame = ui_kit_frame(game->camera.viewport_width, game->camera.viewport_height,
	                       game->settings.ui_scale);
	l.tab_w = (l.frame.w - 280.0f) / (float)SF_TAB_COUNT - SM_TAB_GAP;

	if(l.tab_w > 250.0f)
		l.tab_w = 250.0f;

	tabs_w = l.tab_w * SF_TAB_COUNT + SM_TAB_GAP * (SF_TAB_COUNT - 1);
	l.tab_x = (l.frame.w - tabs_w) * 0.5f;
	l.pw = l.frame.w - 120.0f < 1160.0f ? l.frame.w - 120.0f : 1160.0f;
	l.px = (l.frame.w - l.pw) * 0.5f;
	l.py = SM_PANEL_TOP;
	l.ph = l.frame.h - SM_PANEL_TOP - SM_FOOTER_H;
	l.label_x = l.px + 50.0f;
	l.ctl_x = l.px + l.pw * SM_CTL_FRAC;
	l.ctl_w = l.pw * 0.43f < 500.0f ? l.pw * 0.43f : 500.0f;
	l.btn_x = (l.frame.w - (3.0f * SM_BTN_W + 2.0f * SM_BTN_GAP)) * 0.5f;
	l.btn_y = l.frame.h - 124.0f;

	return l;
}

static float tab_left(const layout* l, int tab) {
	return l->tab_x + (float)tab * (l->tab_w + SM_TAB_GAP);
}

static float row_cy(const layout* l, int n) {
	return l->py + SM_FIRST_ROW + SM_ROW * (float)n;
}

static float button_x(const layout* l, int n) {
	return l->btn_x + (float)n * (SM_BTN_W + SM_BTN_GAP);
}

static int dirty(const GameState* game) {
	return !sf_equal(&s_pending, &game->settings);
}

static void switch_tab(sf_tab tab) {
	if(tab == s_tab)
		return;

	if(s_tab == SF_TAB_KEYBINDS)
		kp_leave();

	s_tab = tab;
	s_dropdown = -1;
	s_drag = -1;

	if(tab == SF_TAB_KEYBINDS)
		kp_enter();
}

void sm_open(const GameSettings* current, sf_tab tab) {
	s_pending = *current;
	s_open = 1;
	s_tab = SF_TAB_COUNT;
	s_confirm = 0;
	switch_tab(tab);
	ui_kit_load();
}

static void close_menu(void) {
	if(s_tab == SF_TAB_KEYBINDS)
		kp_leave();

	s_open = 0;
	s_dropdown = -1;
	s_drag = -1;
	s_confirm = 0;
}

void sm_close(void) {
	if(s_open)
		close_menu();
}

int sm_is_open(void) {
	return s_open;
}

int sm_scroll(float notches) {
	if(!s_open || s_tab != SF_TAB_KEYBINDS || s_confirm)
		return 0;

	return kp_scroll(notches);
}

static void apply(GameState* game) {
	game->settings = s_pending;
	game_settings_save(&game->settings, SETTINGS_PATH);
	game_settings_apply(&game->settings);
}

static void reset_tab(void) {
	if(s_tab == SF_TAB_KEYBINDS)
		keybinds_reset_all();
	else
		sf_reset_tab(&s_pending, s_tab);
}

// The rectangle of a row's control, for both drawing and hit testing
static void control_rect(const layout* l, const sf_row* row, int n, float* x, float* y,
                         float* w, float* h) {
	float cy = row_cy(l, n);

	*y = cy - SM_ROW * 0.5f;
	*h = SM_ROW;

	if(row->kind == SF_TOGGLE) {
		*x = l->label_x;
		*w = l->pw - 100.0f;
	} else {
		*x = l->ctl_x;
		*w = l->ctl_w;
	}
}

static void set_slider_from_pointer(const layout* l, const sf_row* row, float mx) {
	kit_slider_geom g = ui_kit_slider_geom(l->ctl_x, l->ctl_w);
	float t = (mx - g.inner_x) / g.inner_w;

	if(t < 0.0f)
		t = 0.0f;

	if(t > 1.0f)
		t = 1.0f;

	sf_set(&s_pending, row->field, row->lo + t * (row->hi - row->lo));
}

static void click_slider(const layout* l, const sf_row* row, int n, float mx) {
	kit_slider_geom g = ui_kit_slider_geom(l->ctl_x, l->ctl_w);
	float value = sf_get(&s_pending, row->field);

	if(mx < g.track_x)
		sf_set(&s_pending, row->field, value - row->step);
	else if(mx > g.track_x + g.track_w)
		sf_set(&s_pending, row->field, value + row->step);
	else {
		set_slider_from_pointer(l, row, mx);
		s_drag = n;
	}
}

static void click_row(const layout* l, const sf_row* row, int n, float mx) {
	switch(row->kind) {
	case SF_SLIDER:
		click_slider(l, row, n, mx);
		break;
	case SF_TOGGLE:
		sf_set(&s_pending, row->field, sf_get(&s_pending, row->field) < 0.5f ? 1.0f : 0.0f);
		break;
	case SF_CHOICE:
		if(!sf_row_locked(&s_pending, row->field))
			s_dropdown = n;

		break;
	}
}

static void click_rows(const layout* l, float mx, float my) {
	for(int n = 0; n < sf_tab_row_count(s_tab); n++) {
		const sf_row* row = sf_tab_row(s_tab, n);
		float x;
		float y;
		float w;
		float h;

		control_rect(l, row, n, &x, &y, &w, &h);

		if(ui_kit_hit(mx, my, x, y, w, h))
			click_row(l, row, n, mx);
	}
}

// Where a dropdown's list item sits
static float item_y(const layout* l, int row_n, int item) {
	return row_cy(l, row_n) + KIT_FIELD_H * 0.5f + 6.0f + KIT_FIELD_H * (float)item;
}

static void click_dropdown(const layout* l, float mx, float my) {
	const sf_row* row = sf_tab_row(s_tab, s_dropdown);

	for(int i = 0; row && i < sf_choice_count(row->field); i++) {
		if(ui_kit_hit(mx, my, l->ctl_x, item_y(l, s_dropdown, i), l->ctl_w, KIT_FIELD_H))
			sf_choice_set(&s_pending, row->field, i);
	}

	s_dropdown = -1;
}

// Back, or ask before throwing changes away; nonzero when it closed
static int request_back(const GameState* game) {
	if(dirty(game)) {
		s_confirm = 1;
		return 0;
	}

	close_menu();

	return 1;
}

static int click_confirm(const layout* l, float mx, float my) {
	float bx = l->frame.w * 0.5f - SM_BTN_W - SM_BTN_GAP * 0.5f;
	float by = l->frame.h * 0.5f + 40.0f;

	if(ui_kit_hit(mx, my, bx, by, SM_BTN_W, SM_BTN_H)) {
		close_menu();
		return 1;
	}

	if(ui_kit_hit(mx, my, bx + SM_BTN_W + SM_BTN_GAP, by, SM_BTN_W, SM_BTN_H))
		s_confirm = 0;

	return 0;
}

// A click outside any list or prompt; nonzero when the menu closed
static int click_main(GameState* game, const layout* l, float mx, float my) {
	float cy = 22.0f;

	for(int t = 0; t < SF_TAB_COUNT; t++) {
		if(ui_kit_hit(mx, my, tab_left(l, t), cy, l->tab_w, SM_TAB_H))
			switch_tab((sf_tab)t);
	}

	if(ui_kit_hit(mx, my, l->frame.w - 92.0f, cy, SM_CLOSE, SM_CLOSE))
		return request_back(game);

	if(ui_kit_hit(mx, my, button_x(l, 0), l->btn_y, SM_BTN_W, SM_BTN_H))
		return request_back(game);

	if(ui_kit_hit(mx, my, button_x(l, 1), l->btn_y, SM_BTN_W, SM_BTN_H))
		apply(game);

	if(ui_kit_hit(mx, my, button_x(l, 2), l->btn_y, SM_BTN_W, SM_BTN_H))
		reset_tab();

	if(s_tab == SF_TAB_KEYBINDS)
		kp_click(mx, my);
	else
		click_rows(l, mx, my);

	return 0;
}

static int handle_escape(const GameState* game) {
	if(s_dropdown >= 0)
		s_dropdown = -1;
	else if(s_confirm)
		s_confirm = 0;
	else if(!kp_escape())
		return request_back(game);

	return 0;
}

int sm_update(GameState* game) {
	layout l = make_layout(game);
	const InputState* in = &game->input;
	float mx;
	float my;

	if(!s_open)
		return 0;

	ui_kit_to_layout(&l.frame, in->mouse_x, in->mouse_y, &mx, &my);

	if(!in->mouse_left_down)
		s_drag = -1;

	if(input_key_just_pressed(in, GLFW_KEY_ESCAPE))
		return handle_escape(game);

	if(s_tab == SF_TAB_KEYBINDS) {
		kp_set_area(l.px, l.py, l.pw, l.ph);
		kp_poll_keys(mx, my, in);
	}

	if(s_drag >= 0 && in->mouse_left_down && sf_tab_row(s_tab, s_drag))
		set_slider_from_pointer(&l, sf_tab_row(s_tab, s_drag), mx);

	if(!in->mouse_left_clicked)
		return 0;

	if(s_confirm)
		return click_confirm(&l, mx, my);

	if(s_dropdown >= 0) {
		click_dropdown(&l, mx, my);
		return 0;
	}

	return click_main(game, &l, mx, my);
}

static int slider_hover_parts(const layout* l, float mx, float my, float cy) {
	kit_slider_geom g = ui_kit_slider_geom(l->ctl_x, l->ctl_w);
	int parts = 0;

	if(ui_kit_hit(mx, my, g.left_x, cy - KIT_ARROW * 0.5f, KIT_ARROW, KIT_ARROW))
		parts |= KIT_SLIDER_LEFT;

	if(ui_kit_hit(mx, my, g.right_x, cy - KIT_ARROW * 0.5f, KIT_ARROW, KIT_ARROW))
		parts |= KIT_SLIDER_RIGHT;

	return parts;
}

static void draw_control(const layout* l, const sf_row* row, int n, float mx, float my) {
	float cy = row_cy(l, n);
	char text[16];

	if(row->kind == SF_SLIDER) {
		float t = (sf_get(&s_pending, row->field) - row->lo) / (row->hi - row->lo);

		ui_kit_slider(l->ctl_x, cy, l->ctl_w, t, slider_hover_parts(l, mx, my, cy));
		sf_value_text(&s_pending, row->field, text, sizeof(text));
		ui_kit_text(l->ctl_x + l->ctl_w + 24.0f, cy, text, 30.0f, KIT_WHITE, KIT_LEFT);
	} else if(row->kind == SF_CHOICE) {
		ui_kit_field(l->ctl_x, cy, l->ctl_w,
		             sf_choice_label(row->field, sf_choice_get(&s_pending, row->field)),
		             s_dropdown == n);

		// A locked row shows its value greyed out
		if(sf_row_locked(&s_pending, row->field))
			renderer_draw_rect(l->ctl_x, cy - KIT_FIELD_H * 0.5f, l->ctl_w, KIT_FIELD_H + 4.0f,
			                   0.05f, 0.06f, 0.09f, 0.55f);
	} else
		ui_kit_toggle(l->label_x, cy, sf_get(&s_pending, row->field) >= 0.5f);
}

static void draw_row(const layout* l, const sf_row* row, int n, float mx, float my) {
	float cy = row_cy(l, n);
	float top = cy - SM_ROW * 0.5f;
	float label_x = row->kind == SF_TOGGLE ? l->label_x + 52.0f : l->label_x;
	int hot = ui_kit_hit(mx, my, l->px + 20.0f, top, l->pw - 40.0f, SM_ROW);

	if(hot && s_dropdown < 0 && !s_confirm)
		renderer_draw_rect(l->px + 20.0f, top, l->pw - 40.0f, SM_ROW, 1.0f, 1.0f, 1.0f,
		                   0.06f);

	if(sf_row_locked(&s_pending, row->field))
		ui_kit_text(label_x, cy, row->label, 30.0f, KIT_DIM, KIT_LEFT);
	else
		ui_kit_text(label_x, cy, row->label, 30.0f, hot ? KIT_GOLD : KIT_WHITE, KIT_LEFT);

	draw_control(l, row, n, mx, my);

	if(n + 1 < sf_tab_row_count(s_tab))
		ui_kit_divider(l->label_x, cy + SM_ROW * 0.5f, l->pw - 100.0f);
}

static void draw_tabs(const layout* l, float mx, float my) {
	for(int t = 0; t < SF_TAB_COUNT; t++)
		ui_kit_tab(tab_left(l, t), 22.0f, l->tab_w, SM_TAB_H, sf_tab_name((sf_tab)t),
		           t == (int)s_tab, ui_kit_hit(mx, my, tab_left(l, t), 22.0f, l->tab_w, SM_TAB_H));

	ui_kit_close_button(l->frame.w - 92.0f, 22.0f, SM_CLOSE,
	                    ui_kit_hit(mx, my, l->frame.w - 92.0f, 22.0f, SM_CLOSE, SM_CLOSE));
}

static kit_btn_state btn_state(const GameState* game, float mx, float my, float x,
                               float y) {
	if(!ui_kit_hit(mx, my, x, y, SM_BTN_W, SM_BTN_H) || s_confirm)
		return KIT_BTN_NORMAL;

	return game->input.mouse_left_down ? KIT_BTN_DOWN : KIT_BTN_HOVER;
}

static void draw_footer(const GameState* game, const layout* l, float mx, float my) {
	float y = l->btn_y;

	if(dirty(game))
		ui_kit_text(l->frame.w * 0.5f, y - 44.0f, "You have unsaved changes.", 26.0f,
		            KIT_DIM, KIT_CENTER);

	ui_kit_button(button_x(l, 0), y, SM_BTN_W, SM_BTN_H, "Back", 0,
	              btn_state(game, mx, my, button_x(l, 0), y));
	ui_kit_button(button_x(l, 1), y, SM_BTN_W, SM_BTN_H, "Apply", 1,
	              btn_state(game, mx, my, button_x(l, 1), y));
	ui_kit_check_icon(button_x(l, 1) + 60.0f, y + SM_BTN_H * 0.5f - 2.0f);
	ui_kit_button(button_x(l, 2), y, SM_BTN_W, SM_BTN_H, "Reset", 0,
	              btn_state(game, mx, my, button_x(l, 2), y));
}

static void draw_dropdown_list(const layout* l, float mx, float my) {
	const sf_row* row = sf_tab_row(s_tab, s_dropdown);
	int count = row ? sf_choice_count(row->field) : 0;
	int chosen = row ? sf_choice_get(&s_pending, row->field) : 0;

	for(int i = 0; i < count; i++) {
		float y = item_y(l, s_dropdown, i);
		int hot = ui_kit_hit(mx, my, l->ctl_x, y, l->ctl_w, KIT_FIELD_H);

		renderer_draw_rect(l->ctl_x, y, l->ctl_w, KIT_FIELD_H,
		                   hot ? 1.0f : 0.99f, hot ? 0.85f : 0.98f, hot ? 0.55f : 0.94f, 1.0f);
		renderer_draw_rect(l->ctl_x, y + KIT_FIELD_H - 3.0f, l->ctl_w, 3.0f,
		                   1.0f, 0.78f, 0.45f, 1.0f);
		ui_kit_text(l->ctl_x + 20.0f, y + KIT_FIELD_H * 0.5f - 2.0f,
		            sf_choice_label(row->field, i), 26.0f, i == chosen ? KIT_GOLD : KIT_INK,
		            KIT_LEFT);
	}
}

static void draw_confirm(const layout* l, float mx, float my, const GameState* game) {
	float w = 760.0f;
	float h = 300.0f;
	float x = (l->frame.w - w) * 0.5f;
	float y = (l->frame.h - h) * 0.5f - 20.0f;
	float bx = l->frame.w * 0.5f - SM_BTN_W - SM_BTN_GAP * 0.5f;
	float by = l->frame.h * 0.5f + 40.0f;

	ui_kit_dim(0.0f, 0.0f, l->frame.w, l->frame.h, 0.6f);
	renderer_draw_rect(x, y, w, h, 0.05f, 0.05f, 0.09f, 0.97f);
	ui_kit_panel(x, y, w, h);
	ui_kit_text(l->frame.w * 0.5f, y + 70.0f, "Discard unsaved changes?", 34.0f,
	            KIT_GOLD, KIT_CENTER);
	ui_kit_text(l->frame.w * 0.5f, y + 130.0f, "Your changes have not been applied.",
	            26.0f, KIT_DIM, KIT_CENTER);

	ui_kit_button(bx, by, SM_BTN_W, SM_BTN_H, "Discard", 0, btn_state(game, mx, my, bx, by));
	ui_kit_button(bx + SM_BTN_W + SM_BTN_GAP, by, SM_BTN_W, SM_BTN_H, "Keep editing", 1,
	              KIT_BTN_NORMAL);
}

void sm_draw(const GameState* game) {
	layout l = make_layout(game);
	float mx;
	float my;

	if(!s_open)
		return;

	ui_kit_to_layout(&l.frame, game->input.mouse_x, game->input.mouse_y, &mx, &my);
	ui_kit_begin(&l.frame);
	draw_tabs(&l, mx, my);
	ui_kit_panel(l.px, l.py, l.pw, l.ph);

	if(s_tab == SF_TAB_KEYBINDS) {
		kp_set_area(l.px, l.py, l.pw, l.ph);
		kp_draw(s_confirm ? -1.0f : mx, s_confirm ? -1.0f : my);
	} else {
		for(int n = 0; n < sf_tab_row_count(s_tab); n++)
			draw_row(&l, sf_tab_row(s_tab, n), n, mx, my);
	}

	draw_footer(game, &l, mx, my);

	if(s_dropdown >= 0)
		draw_dropdown_list(&l, mx, my);

	if(s_confirm)
		draw_confirm(&l, mx, my, game);

	ui_kit_end();
}
