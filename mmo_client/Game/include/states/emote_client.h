#ifndef EMOTE_CLIENT_H
#define EMOTE_CLIENT_H

// Emotes as the local player lives them: ask, predict, follow the server.

// The rules and wording live in player/emote.h; this wires them to the game.

#include "core/game_types.h"
#include "player/emote.h"
#include "player/emote_slots.h"

// /sit, /dance: check, start the body at once, and ask the server.

// A failed check says "Unable to <emote>" in red in chat.
void emote_client_request(GameState* game, const emote_def* def);

// An Emote key: starts the clip in the slot (EMOTE_SLOT_EMOTE_1 to 3) as an emote.
void emote_client_request_slot(GameState* game, int slot);

// /stand or a movement key: end the player's own emote and tell the server.
void emote_client_stop(GameState* game);

// Applies what the server has said about the player's emote since last frame.
void emote_client_poll(GameState* game);

// Advances the player's emote and everyone else's by one frame.
void emote_client_update(GameState* game, float dt);

// Each CharacterAnim's clip length in seconds, for the emote state machines.
// A clip emote's intro and exit lengths are those of its own clips.
void emote_client_durations(const emote_state* s, float out[CHARACTER_ANIM_COUNT]);

#endif /* EMOTE_CLIENT_H */
