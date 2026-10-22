/**
 * @file
 * Implement the main-menu settings state around the shared settings menu.
 */
#include "state_handler.h"
#include "game.h"
#include "renderer.h"
#include "ui/settings_menu.h"
#include "input/input.h"

#include <stdio.h>
#include "core/client_log.h"

static void settings_enter(GameState* game) {
	CLOG_INFO("[STATE] Entering settings");
	sm_open(&game->settings, SF_TAB_GAMEPLAY);
}

static void settings_exit(GameState* game) {
	(void)game;
	CLOG_INFO("[STATE] Exiting settings");
}

static void settings_update(GameState* game, float delta_time) {
	(void)game; (void)delta_time;
}

/** Render the settings menu over the menu background. */
static void settings_render(GameState* game) {
	int vw = game->camera.viewport_width;
	int vh = game->camera.viewport_height;

	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glOrtho(0, vw, vh, 0, -1, 1);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();

	if(game->textures.background != 0)
		renderer_draw_sprite(0, 0, vw, vh, game->textures.background);
	else
		renderer_draw_rect(0, 0, (float)vw, (float)vh, 0.08f, 0.08f, 0.14f, 1.0f);

	// Dim the background so the menu reads over it
	renderer_draw_rect(0, 0, (float)vw, (float)vh, 0.0f, 0.0f, 0.0f, 0.62f);
	sm_draw(game);
}

/** Take settings input; leaving returns to the main menu. */
static void settings_input(GameState* game, GLFWwindow* window, float delta_time) {
	(void)window; (void)delta_time;

	if(sm_update(game))
		game_change_state(game, GAME_MODE_MAIN_MENU);
}

/** Dispatch lifecycle callbacks for the main-menu settings state. */
const StateHandler g_state_settings = {
	.enter        = settings_enter,
	.exit         = settings_exit,
	.update       = settings_update,
	.render       = settings_render,
	.handle_input = settings_input
};
