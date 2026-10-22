#ifndef SETTINGS_MENU_H
#define SETTINGS_MENU_H

/**
 * @file
 * Declare the full-screen settings menu shared by the main menu and the game.
 *
 * Edits are made to a copy and only reach the game on Apply; leaving with
 * changes unapplied asks first. Key bindings are the exception: they are saved
 * as each one is set.
 */

#include "core/game_types.h"
#include "ui/settings_fields.h"

// Open the menu on a tab, editing a copy of the current settings
void sm_open(const GameSettings* current, sf_tab tab);

// Take this frame's input; nonzero when the menu closed
int sm_update(GameState* game);

void sm_draw(const GameState* game);

// Wheel notches; nonzero when the menu took them
int sm_scroll(float notches);

int sm_is_open(void);

// Shut the menu without applying anything, whatever it was doing
void sm_close(void);

#endif // SETTINGS_MENU_H
