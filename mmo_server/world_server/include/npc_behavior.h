#ifndef NPC_BEHAVIOR_H
#define NPC_BEHAVIOR_H

/** @file Run one NPC's think and act phases (V1, V2, V8, V13, V14).
 *
 * The state machine that used to be the body of npc_ai_tick(): resolve a target,
 * move relative to it, pick an ability that is off cooldown and in range, and
 * describe its delivery into the deferred queue.
 *
 * Five verbs live here because all five are decisions about *this* NPC and its
 * target, taken under the slot lock it already holds:
 *
 *  - V1  contact, a delivery with no telegraph that lands on touch
 *  - V2  multi-shot, which walks its shots across ticks rather than looping
 *  - V8  dash and displacement, which travels rather than teleporting
 *  - V13 target priority, an archetype field over the existing candidate list
 *  - V14 telegraph variants: feints, randomised reach, randomised cast time
 *
 * Everything the act phase decides is *described* into the queue, never performed:
 * a projectile spawn, a zone, a summon and damage against a player all touch
 * state this NPC's lock does not cover. See npc_deferred.h.
 */

#include "npc_think.h"

/** Ensure the act phase's ability shortlist can hold the widest loaded kit.
 *
 * Called once per tick before the scan. Returns 0 when it could not be sized at
 * all, in which case no NPC may choose an ability and the tick does nothing --
 * which is the honest outcome, rather than a truncated kit that reads in game as
 * a mini-boss missing half its abilities.
 */
int npc_behavior_ready(void);

/** Run one living NPC's full think and act phases.
 *
 * The caller holds the pool read lock and this slot's lock, and has filled the
 * context's pool, slot, entity, profile, snapshots, clock and queue. On return
 * the context's target fields describe who this NPC chose, which the trigger
 * evaluator then reads rather than recomputing.
 */
void npc_behavior_think(NPCThink* t);

/** Deliver one ability now, bypassing cooldown, range and phase gates.
 *
 * The path a trigger's `cast` action takes. It is deliberately the same function
 * the act phase calls, so a triggered lunge and a chosen lunge produce the same
 * packets, the same damage and the same recovery -- the only difference being
 * what decided to fire it.
 *
 * @param ab            The ability, already converted to world units.
 * @param ability_slot  Type-local slot to start a cooldown on, or -1 for an
 *                      ability the type does not own (a registry-wide cast).
 * @param aim_x, aim_y  Where to aim; the NPC's own position for a self-cast.
 */
void npc_behavior_cast(NPCThink* t, const NPCAbilityDef* ab, int ability_slot,
                       float aim_x, float aim_y);

/** Advance an in-flight dash and an in-flight burst.
 *
 * Called before the act phase each tick: an NPC mid-dash or mid-volley is
 * committed, and must finish what it started before it may choose again.
 *
 * @return Nonzero when the NPC is committed and the act phase must not run.
 */
int npc_behavior_advance_commitments(NPCThink* t);

#endif // NPC_BEHAVIOR_H
