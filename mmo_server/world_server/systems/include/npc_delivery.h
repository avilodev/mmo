#ifndef NPC_DELIVERY_H
#define NPC_DELIVERY_H

/** @file Describe a chosen NPC ability into the deferred queue.
 *
 * The delivery half of the act phase, split from npc_behavior.c at the seam
 * between deciding *which* ability and describing *what it does*. The decision
 * half reads the world; this half only writes the queue.
 *
 * Every function here is called with the acting NPC's slot lock held.
 */

#include "npc_think.h"

/* --- Modifier-aware ability numbers --------------------------------------
 *
 * Three scalars change per instance: how often an ability may fire, how long it
 * telegraphs, and how hard it lands. Each is read through one function so that a
 * Frenzy, an affix and a Phase Break compose the same way everywhere -- and so
 * that the act phase's "is it ready" test and the cast's "start the cooldown"
 * write can never disagree about the number.
 */

/** Seconds this NPC must wait between uses of an ability, after modifiers. */
double npc_ability_cooldown(const NPCEntity* npc, const NPCAbilityDef* ab);

/** Seconds this NPC telegraphs an ability for, after modifiers. */
double npc_ability_cast_time(const NPCEntity* npc, const NPCAbilityDef* ab);

/** Damage this NPC's ability lands for, before the target's mitigation. */
int npc_ability_damage(const NPCEntity* npc, const NPCAbilityDef* ab);

/** Fill the fields every queued action carries, whatever its kind. */
void npc_delivery_action_begin(const NPCThink* t, NPCDeferredAction* d, NPCDeferredType kind);

/** Queue the optional components an ability carries alongside its delivery.
 *
 * Zones, buffs, heals and summons are components rather than deliveries: an
 * ability may be a telegraph *and* leave a puddle, which a player sees as one
 * attack. They fire when the ability resolves -- at once for an instant
 * delivery, at cast completion for a telegraph.
 */
void npc_delivery_components(NPCThink* t, const NPCAbilityDef* ab,
                             float at_x, float at_y);

#endif // NPC_DELIVERY_H
