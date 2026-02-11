#ifndef STATE_HANDLER_H
#define STATE_HANDLER_H

#include "core/game_types.h"
#include <GLFW/glfw3.h>

// ============================================================================
// STATE HANDLER INTERFACE
// Each game mode implements these functions
// ============================================================================

typedef struct {
    void (*enter)(GameState* game);
    void (*exit)(GameState* game);
    void (*update)(GameState* game, float delta_time);
    void (*render)(GameState* game);
    void (*handle_input)(GameState* game, GLFWwindow* window, float delta_time);
} StateHandler;

// Get handler for a game mode
const StateHandler* state_handler_get(GameMode mode);

// State handler implementations (defined in states/ files)
extern const StateHandler g_state_main_menu;
extern const StateHandler g_state_server_list;
extern const StateHandler g_state_character_select;
extern const StateHandler g_state_playing;

extern GameState* g_current_game;

#endif // STATE_HANDLER_H