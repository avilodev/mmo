#ifndef JOIN_LOADER_H
#define JOIN_LOADER_H

// The loading steps that wait for what the server sends when a character joins.

#include "core/game_types.h"
#include "states/load_queue.h"

// Queue the steps, after the file steps and before the game starts
void join_loader_add_steps(load_queue* q, GameState* game);

// Forget the last join, at the start of the next
void join_loader_reset(void);

// Take a character record into the player, the ability bar and the inventory
void join_apply_character(GameState* game, const CharacterInfo* info);

#endif /* JOIN_LOADER_H */
