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

void network_send_ability_cast(uint16_t ability_id, float aim_x, float aim_y, uint32_t target_id);

void network_send_ability_cancel(void);

#endif // GAME_H