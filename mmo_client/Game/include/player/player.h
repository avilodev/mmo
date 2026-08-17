#ifndef PLAYER_H
#define PLAYER_H

#include "core/game_types.h"

/**
 * @file
 * Declare local player movement, correction, data loading, and rendering.
 */

void player_init(PlayerState* player);

void player_reset_position(PlayerState* player, float x, float y);

int player_update_movement(PlayerState* player, const InputState* input, 
                           const WorldState* world, float delta_time);

void player_apply_correction(PlayerState* player, float x, float y);

void player_load_info(PlayerState* player, const CharacterInfo* info);

void player_render(const PlayerState* player, unsigned int texture, int tile_size);

float player_get_half_size(const PlayerState* player, int tile_size);

#endif // PLAYER_H
