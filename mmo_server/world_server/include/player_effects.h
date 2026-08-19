#ifndef PLAYER_EFFECTS_H
#define PLAYER_EFFECTS_H

/** @file Resolve a player's active effects, race passive, and form into combat state.
 *
 * Everything that changes how much damage a player deals or takes is collected here
 * rather than tracked incrementally. Walking the eight effect slots on each hit is
 * cheap, and it removes an entire class of bug: an accumulator maintained on apply
 * and unwound on remove drifts the moment one path is missed. Nothing to drift.
 *
 * This is also where the race passive resolves, gated on Animal Form. Human Form
 * applies no passive for any race, which is what makes Human Form provably identical
 * across all ten rather than merely intended to be.
 */

#include "ability_def.h"
#include "damage_model.h"
#include "race_registry.h"
#include "server_types.h"

/** Collect every damage modifier applying to a player right now.
 *
 * @param player      The player; NULL clears `out` and returns.
 * @param ally_count  Nearby allies, for passives that scale with them.
 * @param out         Receives the summed modifiers; may not be NULL.
 */
void player_collect_modifiers(const ActivePlayer* player, int ally_count,
                              DamageModifiers* out);

/** Return the race passive in force for a player, or NULL when none applies.
 *
 * Returns NULL in Human Form for every race, and for a race with no passive.
 */
const RacePassive* player_active_passive(const ActivePlayer* player);

/** Report whether a player's actions are locked by a channel.
 *
 * @return Nonzero while an EFFECT_CHANNEL or EFFECT_STUN is active.
 */
int player_is_action_locked(const ActivePlayer* player);

/** Report whether a player's movement is locked.
 *
 * @return Nonzero while an EFFECT_ROOT, EFFECT_CHANNEL or EFFECT_STUN is active.
 */
int player_is_movement_locked(const ActivePlayer* player);

/** Install one effect in a player's first free slot.
 *
 * Instant effects — resource restoration and percentage-of-max healing — are applied
 * immediately here. Timed effects are stored and resolved by the caller's tick.
 *
 * @param source_id  Identifier recorded as the effect's source.
 * @return           1 when the effect was installed or applied, or 0 when no slot was free.
 */
int player_effect_apply(ActivePlayer* player, const AbilityEffectDef* effect,
                        uint32_t source_id);

/** Recompute a player's attributes from the race curve, gear, and active buffs.
 *
 * Called after any change to level, equipment, or buffs. Recomputing from the base
 * curve each time is what keeps a buff that is applied twice and removed once from
 * leaving a permanent bonus behind.
 */
void player_recompute_stats(ActivePlayer* player);

/** Add rage earned from one combat event, clamped to the pool.
 *
 * Rage inverts the usual pool: it builds through combat rather than draining. This
 * is a no-op for a player whose role does not use rage.
 */
void player_add_rage(ActivePlayer* player, int damage_dealt, int damage_taken);

/** Return the ability power multiplier for a player's current form. */
float player_form_power(const ActivePlayer* player);

/** Start a cooldown on one hotbar slot.
 *
 * Stores the instant the slot becomes ready rather than the time remaining. Nothing
 * then has to tick a cooldown down, which is why a cooldown keeps running for the
 * form the player is not in and why no swap sequence can shorten one.
 *
 * @param now       The current monotonic time.
 * @param duration  Seconds until the slot is ready again.
 */
void player_start_cooldown(ActivePlayer* player, uint8_t form, int slot,
                           double now, double duration);

/** Return the seconds remaining on one hotbar slot's cooldown.
 *
 * @return Zero when the slot is ready, or when the form or slot is out of range.
 */
double player_cooldown_remaining(const ActivePlayer* player, uint8_t form, int slot,
                                 double now);

/** Report whether one hotbar slot may be used.
 *
 * @return Nonzero when the slot's cooldown has expired.
 */
int player_slot_is_ready(const ActivePlayer* player, uint8_t form, int slot, double now);

#endif // PLAYER_EFFECTS_H
