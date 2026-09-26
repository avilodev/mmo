#ifndef NPC_EFFECTS_H
#define NPC_EFFECTS_H

/** @file Resolve an NPC's status effects into combat state.
 *
 * The NPC counterpart to player_effects.h, and deliberately its mirror: the two
 * are the same mechanic, and a reader of one should be able to read the other.
 * That includes the rule that matters most -- **collect on read, never
 * accumulate**. Walking a handful of effect slots on each hit is cheap, and it
 * removes an entire class of bug: a running total maintained on apply and unwound
 * on remove drifts the first time a removal path is missed, and an NPC that keeps
 * a dead buff is indistinguishable from a tuning mistake.
 *
 * The one accumulator that does exist is `NPCEntity.mod_*`, and it is not this:
 * those are trigger modifiers, which stack deliberately and permanently for the
 * life of a spawn. Effects expire; trigger modifiers do not.
 *
 * Storage is the pool's per-slot effect array (npc_world_effects()), so the caller
 * holds the slot lock for every function here that takes a slot.
 */

#include "ability_def.h"
#include "damage_model.h"
#include "npc_snapshot.h"
#include "npc_world.h"

#include <stdint.h>

/** Install one effect in an NPC's first free slot, replacing a weaker duplicate.
 *
 * An effect of a type already present is refreshed rather than duplicated when
 * the incoming one is at least as strong: two Rally Howls on one wolf should read
 * as one buff with its timer reset, not as two stacks, which is what a pack of
 * five howling at each other would otherwise produce within a second.
 *
 * Three effect types are not slots at all and are handled here rather than
 * stored: a taunt and a charm redirect target selection, and a cleanse clears.
 * They are the same StatusEffectType the player side uses, applied to the field
 * on NPCEntity that already expresses them.
 *
 * The caller must hold the slot's lock.
 *
 * @param npc        The entity in that slot; may not be NULL.
 * @param source_id  Identifier recorded as the effect's source.
 * @return           1 when installed, applied or refreshed, 0 when no slot is free.
 */
int npc_effect_apply(NPCWorld* world, int slot, NPCEntity* npc,
                     const AbilityEffectDef* effect, uint32_t source_id);

/** Advance one NPC's effect timers by `dt`, applying periodic ticks.
 *
 * The caller must hold the slot's lock. Damage dealt by a damage-over-time effect
 * is written straight to the NPC's health, which is why this runs under the lock
 * rather than through the deferred queue: the target is the NPC already held.
 *
 * @param out_dot_damage  Receives the total damage the tick dealt; may be NULL.
 * @return                1 when the NPC died from a periodic effect this tick.
 */
int npc_effects_tick(NPCWorld* world, int slot, NPCEntity* npc, float dt,
                     int* out_dot_damage);

/** Collect every damage modifier applying to one NPC right now.
 *
 * Sums the NPC's active effects with the accumulated trigger and affix modifiers,
 * then adds its flat armor. The caller must hold the slot's lock.
 */
void npc_collect_modifiers(NPCWorld* world, int slot, const NPCEntity* npc,
                           DamageModifiers* out);

/** Report the NPC's movement speed multiplier from effects and trigger modifiers.
 *
 * A slow of 30% and a Frenzy of +30% resolve in percentage space and apply once,
 * matching damage_model.h's rule, so the two cancel exactly rather than
 * approximately. The caller must hold the slot's lock.
 *
 * @return A multiplier, clamped to a floor of 0.1 so a stacked slow never freezes
 *         an NPC into a permanently un-killable statue in a corner.
 */
float npc_speed_multiplier(NPCWorld* world, int slot, const NPCEntity* npc);

/** Report whether this NPC may act at all.
 *
 * @return Nonzero while an EFFECT_STUN or EFFECT_CHANNEL holds it.
 */
int npc_is_action_locked(NPCWorld* world, int slot);

/** Report whether this NPC may move.
 *
 * @return Nonzero while an EFFECT_ROOT, EFFECT_STUN or EFFECT_CHANNEL holds it,
 *         unless its archetype is movement_cc_immune -- which is what `relentless`
 *         means and why the Ancient Guardian keeps walking through a root.
 */
int npc_is_movement_locked(NPCWorld* world, int slot, int cc_immune);

/* --- Ally queries (V12) --------------------------------------------------
 *
 * Answered against the tick's NPC snapshot rather than the pool, for the reason
 * npc_snapshot.h gives: the snapshot is the right thing to *select* with and the
 * wrong thing to mutate through. Every one of these returns dense snapshot
 * indices, which the caller resolves through npc_world_acquire_slot().
 */

/** Find living allies of one NPC within a radius.
 *
 * "Ally" means another living NPC, optionally restricted to the same faction --
 * Rally Howl buffs wolves, not everything standing nearby.
 *
 * @param self_id        The asking NPC, always excluded from the result.
 * @param faction_index  Restrict to this faction, or -1 for any.
 * @param out_indices    Receives nearest-first dense snapshot indices.
 * @return               How many were written, at most max_out.
 */
int npc_allies_near(NPCTickSnapshot* npcs, float x, float y, float radius,
                    uint32_t self_id, int faction_index,
                    int* out_indices, int max_out);

/** Report how many allies are below a health fraction, for `allies_below`.
 *
 * @param fraction  0.5 means "at or below half health".
 */
int npc_allies_below_health(NPCTickSnapshot* npcs, float x, float y, float radius,
                            uint32_t self_id, int faction_index, float fraction);

/** Pick the ally with the lowest health fraction, for Rogue Deer's mend.
 *
 * @return A dense snapshot index, or -1 when no ally is in range or none is hurt.
 */
int npc_lowest_health_ally(NPCTickSnapshot* npcs, float x, float y, float radius,
                           uint32_t self_id, int faction_index);

/* --- Stealth (V10) --------------------------------------------------------
 *
 * Enforced by omission from the broadcast rather than by a client-side flag: a
 * hidden NPC is simply not in the stream, so no client has its position to draw
 * whatever it is told to draw. That is the only implementation of stealth that
 * survives someone reading the packets.
 */

/** Report whether an NPC is currently hidden from the broadcast.
 *
 * The caller must hold the slot lock, or be the broadcast pass holding it.
 */
int npc_stealth_hidden(const NPCEntity* npc, double now);

/** Reveal a stealthed NPC for its type's reveal window.
 *
 * Called wherever a stealthed NPC gives itself away: taking damage, and acting.
 * A no-op for a type that does not stealth, so callers need no predicate.
 */
void npc_stealth_reveal(NPCEntity* npc, double now);

/** Report whether damage may be applied to an NPC at all.
 *
 * A stealthed type may declare itself untargetable while hidden, which is how a
 * Snake in ambush differs from one merely standing in tall grass.
 */
int npc_stealth_damageable(const NPCEntity* npc, double now);

/** Resolve an NPC type's faction index without holding any lock.
 *
 * The registry is immutable after load, so this is a plain table read. Returns -1
 * for a type the registry does not know, which is how quest and system NPCs --
 * which have no faction -- fall out of every faction-scoped query.
 */
int npc_faction_of_type(uint16_t npc_type_id);

#endif // NPC_EFFECTS_H
