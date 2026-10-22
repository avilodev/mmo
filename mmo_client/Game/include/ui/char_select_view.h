#ifndef CHAR_SELECT_VIEW_H
#define CHAR_SELECT_VIEW_H

/**
 * @file
 * Draw the character select scene and read its input.
 *
 * The selected character stands in the middle, their class details run down the
 * left, the top right cycles through the characters, and the last entry of the
 * cycle is a new character. The bottom right button joins the world.
 */

#include "core/game_types.h"

// What the player asked for this frame; the selected index says which character
typedef enum {
	CSV_NONE,
	CSV_JOIN,   // enter the world as the selected character
	CSV_DELETE, // delete the selected character, after the prompt
	CSV_NEW,    // open character creation
	CSV_BACK,   // leave to the server list
} csv_action;

// Start a visit: no prompt up
void csv_open(void);

// Free what the scene draws with
void csv_close(void);

// Draw the selected character into their picture; call before the frame's UI
void csv_render_preview(const GameState* game, float dt);

// Read the frame's input. `busy` is a request in flight: it blocks everything
// but going back.
csv_action csv_update(GameState* game, int busy);

// Escape closes the delete prompt; returns 1 when it did, so the key is spent
int csv_escape(void);

// Draw the scene; `status` is shown in the middle when not NULL
void csv_draw(const GameState* game, const char* status);

#endif /* CHAR_SELECT_VIEW_H */
