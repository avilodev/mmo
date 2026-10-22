#ifndef PAUSE_MENU_H
#define PAUSE_MENU_H

/**
 * @file
 * Declare the pause menu: a column of buttons down the left of the dimmed world.
 */

#include "core/game_types.h"

typedef enum {
	PM_NONE,
	PM_RESUME,
	PM_SETTINGS,
	PM_QUIT,        // confirmed: leave for the main menu
} pause_action;

// Call as the menu appears
void pm_open(void);

// Take this frame's input; the action chosen, or PM_NONE
pause_action pm_update(const GameState* game);

void pm_draw(const GameState* game);

#endif // PAUSE_MENU_H
