#ifndef PLAYER_CAST_FLAGS_H
#define PLAYER_CAST_FLAGS_H

/** @file Publish "this player is mid-cast" for the NPC trigger evaluator.
 *
 * `ability_cast_nearby` -- the Duelist affix's counter window, and every future
 * enemy that punishes a player for committing to a cast -- needs to know, from
 * the gameplay thread, whether a nearby player is casting. The two authoritative
 * answers live in `g_ability_casts` and `g_pending_casts`, both private to their
 * modules and both written from network threads.
 *
 * Rather than thread a predicate out of each, or set a flag at each of the eleven
 * places a cast starts, cancels or resolves, each module republishes this once per
 * tick from its own authoritative array. One write site per module means the flag
 * cannot drift out of step with the truth: a missed clear is impossible when the
 * clear is a fresh read of the source rather than a matching edit.
 *
 * The cost is that the answer is at most one tick -- fifty milliseconds -- stale,
 * which no counter window measured in half-seconds can tell.
 */

#include "types.h"

#include <stdatomic.h>
#include <stdint.h>

/** One flag per player slot. Written by the gameplay thread, read by it too;
 * atomic because a reader on another thread is a change away and a torn read of
 * this would be a silent one. */
extern _Atomic uint8_t g_player_casting[MAX_PLAYERS];

/** Publish whether the player in a slot is mid-cast. */
void player_cast_flag_set(int slot, int casting);

/** Report whether the player in a slot was mid-cast as of the last publish. */
int player_cast_flag_get(int slot);

#endif // PLAYER_CAST_FLAGS_H
