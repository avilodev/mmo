#ifndef INSPECT_CLIENT_H
#define INSPECT_CLIENT_H

// Asks the server about another player's gear and stats, and opens the window.

#include "core/game_types.h"

// Ask about the player currently targeted. With none, says so in chat.
void inspect_client_request_target(GameState* game);

// Applies the answer, if one has come: opens the window, or says why not in chat.
void inspect_client_poll(GameState* game);

// Closes the inspect window and forgets a question still waiting for its answer.
void inspect_client_close(GameState* game);

#endif /* INSPECT_CLIENT_H */
