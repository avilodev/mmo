#ifndef STATUS_CLIENT_H
#define STATUS_CLIENT_H

// Follows the status the server shows for the player, and says when it changes.

#include "core/game_types.h"

// Applies what the server has said since last frame: remembers the status and
// writes "You are now Do Not Disturb." (or the like) in chat, once per change.
void status_client_poll(GameState* game);

#endif /* STATUS_CLIENT_H */
