#ifndef PLAYER_H
#define PLAYER_H

#include "core/game_types.h"

// ============================================================================
// PLAYER MODULE
// Handles player state, movement, and rendering
// ============================================================================

// Initialize player state
void player_init(PlayerState* player);

// Reset player to server position
void player_reset_position(PlayerState* player, float x, float y);

// Update player movement based on input
// Returns 1 if player moved, 0 if stationary
int player_update_movement(PlayerState* player, const InputState* input, 
                           const WorldState* world, float delta_time);

// Apply server position correction
void player_apply_correction(PlayerState* player, float x, float y);

// Load character info from server response
void player_load_info(PlayerState* player, const CharacterInfo* info);

// Render player sprite
void player_render(const PlayerState* player, unsigned int texture, int tile_size);

// Get player bounding box half-size
float player_get_half_size(const PlayerState* player, int tile_size);

#endif // PLAYER_H