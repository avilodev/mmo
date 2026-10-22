#ifndef SESSION_RESET_H
#define SESSION_RESET_H

// Put back what a session leaves behind, so the next starts as the first did:
// open windows, the camera, the player's motion, character bodies, caches.
// Settings and keybinds are the player's, not the session's, and are kept.

#include "core/game_types.h"

void session_reset(GameState* game);

#endif /* SESSION_RESET_H */
