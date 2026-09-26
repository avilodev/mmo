#ifndef NPC_MITIGATION_H
#define NPC_MITIGATION_H

/** @file Gate damage against an NPC on a shield, a facing, or a position (V9).
 *
 * The enemies this exists for -- Frost Warden, Cult Enforcer, Iron Zealot, Vault
 * Guardian, Ancient Guardian, Rogue Bear -- are the ones that demand a *specific*
 * answer rather than more damage. Each says the same thing in a different
 * dialect: "not from there", "not until you have done X", "not while this is up".
 *
 * All three reduce to one question asked at the moment a hit lands: given where
 * the attacker is standing and what this NPC's shield has been through, what
 * fraction of this damage actually happens? So there is one function, called from
 * each of the four places NPC health is written, and no enemy's name in the file.
 *
 * A shield is per-instance state, not a stat, which is why it lives on NPCEntity
 * and is cleared with the rest of a slot's state on respawn: an Iron Zealot that
 * came back with its shield already broken would be a different enemy the second
 * time you met it.
 */

#include "npc_world.h"

#include <stdint.h>

/** Resolve one incoming hit against an NPC's mitigation.
 *
 * Updates the shield's break state as a side effect, which is the point: a
 * break window counts hits, and a shield pool absorbs them, so the gate and the
 * bookkeeping are the same operation and cannot be called out of step.
 *
 * The caller must hold this NPC's slot lock.
 *
 * @param damage          Damage after the attacker's modifiers and this NPC's armour.
 * @param from_x, from_y  Where the attack came from, for facing and positional gates.
 * @param now             Monotonic clock, for timed break windows.
 * @param out_blocked     Receives 1 when the hit was reduced by a gate; may be NULL.
 * @return                The damage to actually apply, never negative.
 */
int npc_mitigation_apply(NPCWorld* world, int slot, NPCEntity* npc,
                         int damage, float from_x, float from_y,
                         double now, int* out_blocked);

/** Report whether an NPC's shield is currently up.
 *
 * Exposed so the broadcast can tell a client to draw one. The caller must hold
 * the slot lock.
 */
int npc_mitigation_shield_up(const NPCEntity* npc);

/** Initialise an NPC's shield state from its type, at spawn.
 *
 * The caller must hold the slot lock, or hold the NPC exclusively at spawn.
 */
void npc_mitigation_init(NPCEntity* npc, uint16_t npc_type_id);

#endif // NPC_MITIGATION_H
