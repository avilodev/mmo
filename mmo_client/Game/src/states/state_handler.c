/**
 * @file
 * Map client game modes to their state-handler tables.
 */

#include "state_handler.h"

/**
 * Select the handler table for a game mode.
 *
 * @return      Matching handler, or the main-menu handler for an unknown mode.
 */
const StateHandler* state_handler_get(GameMode mode) {
    switch (mode) {
        case GAME_MODE_MAIN_MENU:        return &g_state_main_menu;
        case GAME_MODE_SERVER_LIST:      return &g_state_server_list;
        case GAME_MODE_CHARACTER_SELECT: return &g_state_character_select;
        case GAME_MODE_PLAYING:          return &g_state_playing;
        case GAME_MODE_SETTINGS:         return &g_state_settings;
        default:                         return &g_state_main_menu;
    }
} 
