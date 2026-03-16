#ifndef GAME_H
#define GAME_H

#include <GLFW/glfw3.h>
#include "core/game_types.h"

// ============================================================================
// GAME MODULE
// Main game interface - thin wrapper that delegates to state handlers
// ============================================================================

// Initialize the game
void game_init(GameState* game, int viewport_width, int viewport_height);

// Handle input for current state
void game_handle_input(GameState* game, GLFWwindow* window, float delta_time);

// Update current state
void game_update(GameState* game, float delta_time);

// Render current state
void game_render(GameState* game);

// Clean up all game resources
void game_cleanup(GameState* game);

// Change game state (handles enter/exit callbacks)
void game_change_state(GameState* game, GameMode new_mode);

// Global window handle — set in main.c after window creation.
// Used by game_settings_apply to toggle fullscreen.
extern GLFWwindow* g_window;

#define SETTINGS_PATH "Game/data/settings.cfg"

// Settings persistence
void game_settings_save(const GameSettings* s, const char* path);
void game_settings_load(GameSettings* s, const char* path);
// Apply settings values to subsystems (audio volumes, fullscreen, etc.)
void game_settings_apply(const GameSettings* s);

void network_send_ability_cast(uint16_t ability_id, float aim_x, float aim_y, uint32_t target_id);

void network_send_ability_cancel(void);

#endif // GAME_H