/**
 * @file
 * Draw the lost-connection dialog and read its input.
 */
#include "ui/lost_connection.h"
#include "ui/ui_kit.h"
#include "input/input.h"
#include "renderer.h"

#define LOST_CONNECTION_W      760.0f
#define LOST_CONNECTION_H      320.0f
#define LOST_CONNECTION_BTN_W  280.0f
#define LOST_CONNECTION_BTN_H  66.0f

static int s_open;
static int s_login_ended;

typedef struct {
	kit_frame frame;
	float x;        // left of the dialog
	float y;        // top of the dialog
	float btn_x;
	float btn_y;
} layout;

static layout make_layout(const GameState* game) {
	layout l;

	l.frame = ui_kit_frame(game->camera.viewport_width, game->camera.viewport_height,
	                       game->settings.ui_scale);
	l.x = (l.frame.w - LOST_CONNECTION_W) * 0.5f;
	l.y = (l.frame.h - LOST_CONNECTION_H) * 0.5f - 20.0f;
	l.btn_x = (l.frame.w - LOST_CONNECTION_BTN_W) * 0.5f;
	l.btn_y = l.y + LOST_CONNECTION_H - LOST_CONNECTION_BTN_H - 40.0f;

	return l;
}

void lost_connection_show(int login_ended) {
	s_open = 1;
	s_login_ended = login_ended;
}

int lost_connection_is_open(void) {
	return s_open;
}

int lost_connection_login_ended(void) {
	return s_open && s_login_ended;
}

lost_connection_action lost_connection_update(const GameState* game) {
	layout l = make_layout(game);
	const InputState* in = &game->input;
	float mx;
	float my;
	int pick = input_key_just_pressed(in, GLFW_KEY_ENTER) ||
	           input_key_just_pressed(in, GLFW_KEY_ESCAPE);

	ui_kit_to_layout(&l.frame, in->mouse_x, in->mouse_y, &mx, &my);

	if(in->mouse_left_clicked && ui_kit_hit(mx, my, l.btn_x, l.btn_y, LOST_CONNECTION_BTN_W, LOST_CONNECTION_BTN_H))
		pick = 1;

	if(!pick)
		return LOST_CONNECTION_NONE;

	s_open = 0;

	return s_login_ended ? LOST_CONNECTION_CLOSE_GAME : LOST_CONNECTION_DISMISSED;
}

void lost_connection_draw(const GameState* game) {
	layout l;
	float mx;
	float my;
	kit_btn_state state = KIT_BTN_NORMAL;

	if(!s_open)
		return;

	l = make_layout(game);
	ui_kit_to_layout(&l.frame, game->input.mouse_x, game->input.mouse_y, &mx, &my);

	if(ui_kit_hit(mx, my, l.btn_x, l.btn_y, LOST_CONNECTION_BTN_W, LOST_CONNECTION_BTN_H))
		state = game->input.mouse_left_down ? KIT_BTN_DOWN : KIT_BTN_HOVER;

	ui_kit_begin(&l.frame);
	ui_kit_dim(0.0f, 0.0f, l.frame.w, l.frame.h, 0.72f);
	renderer_draw_rect(l.x, l.y, LOST_CONNECTION_W, LOST_CONNECTION_H, 0.05f, 0.05f, 0.09f, 0.97f);
	ui_kit_panel(l.x, l.y, LOST_CONNECTION_W, LOST_CONNECTION_H);

	if(s_login_ended) {
		ui_kit_text(l.frame.w * 0.5f, l.y + 70.0f, "Login Ended", 34.0f, KIT_GOLD, KIT_CENTER);
		ui_kit_text(l.frame.w * 0.5f, l.y + 125.0f, "Your login has expired.", 26.0f, KIT_DIM,
		            KIT_CENTER);
		ui_kit_text(l.frame.w * 0.5f, l.y + 160.0f, "Close the game and log in again.", 26.0f,
		            KIT_DIM, KIT_CENTER);
	} else {
		ui_kit_text(l.frame.w * 0.5f, l.y + 70.0f, "Lost Connection", 34.0f, KIT_GOLD,
		            KIT_CENTER);
		ui_kit_text(l.frame.w * 0.5f, l.y + 125.0f, "The connection to the world was lost.",
		            26.0f, KIT_DIM, KIT_CENTER);
		ui_kit_text(l.frame.w * 0.5f, l.y + 160.0f, "Reconnecting to the realm...", 26.0f,
		            KIT_DIM, KIT_CENTER);
	}

	ui_kit_button(l.btn_x, l.btn_y, LOST_CONNECTION_BTN_W, LOST_CONNECTION_BTN_H, s_login_ended ? "Close Game" : "OK", 1,
	              state);
	ui_kit_end();
}
