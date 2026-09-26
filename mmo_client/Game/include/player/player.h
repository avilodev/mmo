#ifndef PLAYER_H
#define PLAYER_H

#include "core/game_types.h"
#include "player/paperdoll.h"

#include <stdint.h>

/**
 * @file
 * Declare local player movement, correction, data loading, and rendering.
 */

void player_init(PlayerState* player);

void player_reset_position(PlayerState* player, float x, float y);

/** Walk the player from the movement keys, resolving collision per axis.
 *
 * @param input_yaw  Camera yaw in radians: the keys are relative to the camera,
 *                   so W walks away from it. Zero reproduces the fixed
 *                   north-up controls of the top-down view.
 * @return Nonzero when the player moved.
 */
int player_update_movement(PlayerState* player, const InputState* input, 
                           const WorldState* world, float delta_time, float input_yaw);

/** Record a position proposal so a correction naming it can be reconciled.
 *
 * Call once for every PlayerMovePacket sent, with the sequence it carried and
 * the position it claimed.
 */
void player_record_sent_move(PlayerState* player, uint32_t sequence,
                             float x, float y);

/** Rewind to the server's position and replay what was sent after it.
 *
 * The server refuses a move by naming it. Everything the client sent after
 * that sequence is still unresolved, so the honest answer is not "you are at
 * (x, y)" -- that was true several frames ago -- but "you are at (x, y) plus
 * whatever you have done since", which is what this computes.
 *
 * The replayed tail is walked against the world rather than added, so a rewind
 * cannot put the player inside a wall or step them through one.
 *
 * When the correction agrees with what the client predicted, the tail lands
 * exactly where the player already is and nothing moves -- which is the point,
 * and is most corrections.
 *
 * @param world     For the collision the replay is checked against.
 * @param sequence  The refused proposal, from network_get_server_correction().
 * @return Nonzero when the player visibly moved, zero when the replay agreed
 *         with the prediction and the correction was invisible.
 */
int player_apply_correction(PlayerState* player, const WorldState* world,
                            uint32_t sequence, float x, float y);

void player_load_info(PlayerState* player, const CharacterInfo* info);

/** Draw the local player's character stack and its name label.
 *
 * @param doll  Appearance to draw; NULL or unloaded falls back to a plain box
 *              so a missing asset is a visible placeholder, not an invisible
 *              player.
 */
void player_render(const PlayerState* player, const Paperdoll* doll, int tile_size);

float player_get_half_size(const PlayerState* player, int tile_size);

#endif // PLAYER_H
