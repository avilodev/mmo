#ifndef GAME_H
#define GAME_H

#include <GLFW/glfw3.h>
#include "core/game_types.h"

/**
 * @file
 * Declare the client lifecycle and state-handler dispatch interface.
 */

void game_init(GameState* game, int viewport_width, int viewport_height);

void game_handle_input(GameState* game, GLFWwindow* window, float delta_time);

void game_update(GameState* game, float delta_time);

void game_render(GameState* game);

void game_cleanup(GameState* game);

/** Exit the current mode and enter new_mode. */
void game_change_state(GameState* game, GameMode new_mode);

/** Window handle assigned by main and used for display-mode changes. */
extern GLFWwindow* g_window;

/** Default settings persistence path. */
#define SETTINGS_PATH "Game/data/settings.cfg"

void game_settings_save(const GameSettings* s, const char* path);
void game_settings_load(GameSettings* s, const char* path);
void game_settings_apply(const GameSettings* s);

void network_send_ability_cast(uint16_t ability_id, float aim_x, float aim_y, uint32_t target_id);

void network_send_ability_cancel(void);

#endif // GAME_H
