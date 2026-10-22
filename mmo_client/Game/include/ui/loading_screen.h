#ifndef LOADING_SCREEN_H
#define LOADING_SCREEN_H

// The screen shown while the game world loads.

#include "core/game_types.h"

// Draw the backdrop, a progress bar and the step in hand.
// A non-NULL error replaces the bar's label and offers the way out.
void loading_screen_draw(const GameState* game, float progress, const char* label,
                         const char* error);

#endif /* LOADING_SCREEN_H */
