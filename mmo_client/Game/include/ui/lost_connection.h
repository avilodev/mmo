#ifndef LOST_CONNECTION_H
#define LOST_CONNECTION_H

/**
 * @file
 * Declare the dialog shown when a dropped connection could not be recovered.
 *
 * It is drawn over whichever screen the player was sent back to, and takes the
 * input while it is up. Two cases: the connection was lost and the player can
 * simply carry on from the menu, or the login itself has ended and the only
 * way forward is to close the game and log in again from the Launcher.
 */

#include "core/game_types.h"

typedef enum {
	LOST_CONNECTION_NONE,
	LOST_CONNECTION_DISMISSED,    // the player acknowledged it
	LOST_CONNECTION_CLOSE_GAME,   // the player must log in again: close the game
} lost_connection_action;

// Open the dialog. login_ended is nonzero when logging in again is the only way back.
void lost_connection_show(int login_ended);

int lost_connection_is_open(void);

// Whether the dialog is up because the login ended, so reconnecting is pointless
int lost_connection_login_ended(void);

// Take this frame's input; the action chosen, or LOST_CONNECTION_NONE
lost_connection_action lost_connection_update(const GameState* game);

void lost_connection_draw(const GameState* game);

#endif // LOST_CONNECTION_H
