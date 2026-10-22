#ifndef EMOTE_HUB_H
#define EMOTE_HUB_H

/**
 * @file
 * Declare the emote hub: the window where the six clips are picked.
 *
 * The slots (Walk, Run, Sit, Emote 1 to 3) are on the left, each with the body
 * playing its clip. Every clip is on the right; dragging one onto a slot puts
 * it there.
 */

#include "core/game_types.h"

// Opens the window; the game takes no input but the window's while it is open
void emote_hub_open(void);
void emote_hub_close(void);
int emote_hub_is_open(void);

// Closed, nothing held, scrolled to the top; for leaving a session
void emote_hub_reset(void);

// Takes this frame's input while open, and keeps the game from seeing it
void emote_hub_update(GameState* game);

// Scrolls the grid for a mouse wheel turn; 1 when it took the turn
int emote_hub_scroll(float wheel);

// Draws the bodies in the boxes; call before emote_hub_draw, outside 3D passes
void emote_hub_render_previews(const GameState* game);

void emote_hub_draw(const GameState* game);

#endif /* EMOTE_HUB_H */
