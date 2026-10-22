/**
 * @file
 * Draw the pause menu and read its input.
 */
#include "ui/pause_menu.h"
#include "ui/ui_kit.h"
#include "input/input.h"
#include "renderer.h"

#define PM_COLUMN_W    320.0f
#define PM_BTN_H       56.0f
#define PM_BTN_GAP     14.0f
#define PM_CONFIRM_W   280.0f
#define PM_CONFIRM_H   66.0f

typedef enum {
	PM_BTN_RESUME,
	PM_BTN_SETTINGS,
	PM_BTN_QUIT,
	PM_BTN_COUNT,
} pause_button;

static const char* const LABELS[PM_BTN_COUNT] = {
	"Resume", "Settings", "Quit to Menu"
};

static int s_focus;        // button the keyboard or pointer is on
static int s_confirm;      // the quit prompt is up
static int s_confirm_focus;

typedef struct {
	kit_frame frame;
	float x;           // left of the column
	float first_y;     // top of the first button
} layout;

static layout make_layout(const GameState* game) {
	layout l;
	float total = PM_BTN_H * PM_BTN_COUNT + PM_BTN_GAP * (PM_BTN_COUNT - 1);

	l.frame = ui_kit_frame(game->camera.viewport_width, game->camera.viewport_height,
	                       game->settings.ui_scale);
	l.x = (l.frame.w - PM_COLUMN_W) * 0.5f;
	l.first_y = (l.frame.h - total) * 0.5f;

	return l;
}

static float button_y(const layout* l, int n) {
	return l->first_y + (PM_BTN_H + PM_BTN_GAP) * (float)n;
}

static float confirm_x(const layout* l, int n) {
	return l->frame.w * 0.5f - PM_CONFIRM_W - PM_BTN_GAP * 0.5f +
	       (PM_CONFIRM_W + PM_BTN_GAP) * (float)n;
}

static float confirm_y(const layout* l) {
	return l->frame.h * 0.5f + 40.0f;
}

void pm_open(void) {
	s_focus = PM_BTN_RESUME;
	s_confirm = 0;
	s_confirm_focus = 0;
}

static pause_action activate(int button) {
	switch(button) {
	case PM_BTN_RESUME:
		return PM_RESUME;
	case PM_BTN_SETTINGS:
		return PM_SETTINGS;
	default:
		s_confirm = 1;
		s_confirm_focus = 0;
		return PM_NONE;
	}
}

static pause_action update_confirm(const layout* l, const InputState* in, float mx,
                                   float my) {
	int pick = -1;

	for(int n = 0; n < 2; n++) {
		if((in->mouse_dx != 0.0f || in->mouse_dy != 0.0f) &&
		   ui_kit_hit(mx, my, confirm_x(l, n), confirm_y(l), PM_CONFIRM_W, PM_CONFIRM_H))
			s_confirm_focus = n;
	}

	if(input_key_just_pressed(in, GLFW_KEY_LEFT))
		s_confirm_focus = 0;

	if(input_key_just_pressed(in, GLFW_KEY_RIGHT))
		s_confirm_focus = 1;

	if(input_key_just_pressed(in, GLFW_KEY_ENTER))
		pick = s_confirm_focus;

	if(in->mouse_left_clicked) {
		for(int n = 0; n < 2; n++) {
			if(ui_kit_hit(mx, my, confirm_x(l, n), confirm_y(l), PM_CONFIRM_W, PM_CONFIRM_H))
				pick = n;
		}
	}

	if(pick < 0)
		return PM_NONE;

	s_confirm = 0;

	return pick == 1 ? PM_QUIT : PM_NONE;
}

pause_action pm_update(const GameState* game) {
	layout l = make_layout(game);
	const InputState* in = &game->input;
	float mx;
	float my;

	ui_kit_to_layout(&l.frame, in->mouse_x, in->mouse_y, &mx, &my);

	if(input_key_just_pressed(in, GLFW_KEY_ESCAPE)) {
		if(!s_confirm)
			return PM_RESUME;

		s_confirm = 0;
		return PM_NONE;
	}

	if(s_confirm)
		return update_confirm(&l, in, mx, my);

	// The pointer takes the focus only when it moves, so the keys still work
	// with it resting on a button
	for(int n = 0; n < PM_BTN_COUNT; n++) {
		if((in->mouse_dx != 0.0f || in->mouse_dy != 0.0f) &&
		   ui_kit_hit(mx, my, l.x, button_y(&l, n), PM_COLUMN_W, PM_BTN_H))
			s_focus = n;
	}

	if(input_key_just_pressed(in, GLFW_KEY_DOWN))
		s_focus = (s_focus + 1) % PM_BTN_COUNT;

	if(input_key_just_pressed(in, GLFW_KEY_UP))
		s_focus = (s_focus + PM_BTN_COUNT - 1) % PM_BTN_COUNT;

	if(input_key_just_pressed(in, GLFW_KEY_ENTER))
		return activate(s_focus);

	if(!in->mouse_left_clicked)
		return PM_NONE;

	for(int n = 0; n < PM_BTN_COUNT; n++) {
		if(ui_kit_hit(mx, my, l.x, button_y(&l, n), PM_COLUMN_W, PM_BTN_H)) {
			s_focus = n;
			return activate(n);
		}
	}

	return PM_NONE;
}

static void draw_confirm(const GameState* game, const layout* l, float mx, float my) {
	float w = 760.0f;
	float h = 300.0f;
	float x = (l->frame.w - w) * 0.5f;
	float y = (l->frame.h - h) * 0.5f - 20.0f;

	ui_kit_dim(0.0f, 0.0f, l->frame.w, l->frame.h, 0.72f);
	renderer_draw_rect(x, y, w, h, 0.05f, 0.05f, 0.09f, 0.97f);
	ui_kit_panel(x, y, w, h);
	ui_kit_text(l->frame.w * 0.5f, y + 70.0f, "Quit to the main menu?", 34.0f, KIT_GOLD,
	            KIT_CENTER);
	ui_kit_text(l->frame.w * 0.5f, y + 130.0f, "You will leave the world.", 26.0f,
	            KIT_DIM, KIT_CENTER);

	for(int n = 0; n < 2; n++) {
		kit_btn_state state = KIT_BTN_NORMAL;

		if(ui_kit_hit(mx, my, confirm_x(l, n), confirm_y(l), PM_CONFIRM_W, PM_CONFIRM_H))
			state = game->input.mouse_left_down ? KIT_BTN_DOWN : KIT_BTN_HOVER;

		ui_kit_button(confirm_x(l, n), confirm_y(l), PM_CONFIRM_W, PM_CONFIRM_H,
		              n == 0 ? "Back" : "Quit", n == s_confirm_focus, state);
	}
}

void pm_draw(const GameState* game) {
	layout l = make_layout(game);
	float mx;
	float my;

	ui_kit_to_layout(&l.frame, game->input.mouse_x, game->input.mouse_y, &mx, &my);
	ui_kit_begin(&l.frame);
	ui_kit_dim(0.0f, 0.0f, l.frame.w, l.frame.h, 0.55f);

	for(int n = 0; n < PM_BTN_COUNT; n++) {
		kit_btn_state state = KIT_BTN_NORMAL;

		if(n == s_focus && game->input.mouse_left_down &&
		   ui_kit_hit(mx, my, l.x, button_y(&l, n), PM_COLUMN_W, PM_BTN_H))
			state = KIT_BTN_DOWN;

		ui_kit_button(l.x, button_y(&l, n), PM_COLUMN_W, PM_BTN_H, LABELS[n],
		              n == s_focus && !s_confirm, state);
	}

	if(s_confirm)
		draw_confirm(game, &l, mx, my);

	ui_kit_end();
}
