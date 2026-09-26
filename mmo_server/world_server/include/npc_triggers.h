#ifndef NPC_TRIGGERS_H
#define NPC_TRIGGERS_H

/** @file Evaluate the reactive trigger table and the phase machine (V16).
 *
 * `enemy_types.txt` describes threshold phases, on-death spawns, counter windows,
 * panic flees and proximity ambushes as if they were nine unrelated mechanics.
 * They differ only in *what fires them*, so there is one table and one evaluator
 * here rather than nine branches in the AI loop -- which is also why V4 does not
 * exist as separate work: a threshold latch is this with one `when` value.
 *
 * Fourteen behaviours in the source document land here: Frenzy, Death Frenzy,
 * Berserk, Curse Empower, Phase Break, Frenzied Slash, Escalation, Panic Flee,
 * Double Bite, Armor Break, Territorial Aggro, Death Release, Ambush and the
 * Rabbit's Evasive Hop. None of them is named in this file.
 *
 * Latches live in the pool's per-NPC bitset, not in the registry, because the
 * registry is immutable and shared by every instance of a type: one Rusher
 * reaching 30% health must not put every Rusher into Frenzy.
 */

#include "npc_think.h"

/** Evaluate every trigger a living NPC declares, firing those whose event holds.
 *
 * Called with the slot lock held, after target selection so that `proximity` and
 * the phase machine can read the resolved target. Actions that touch anything
 * other than this NPC are appended to the queue rather than performed.
 */
void npc_triggers_evaluate(NPCThink* t);

/** Evaluate the `on_death` triggers of an NPC that has just died.
 *
 * Separate because a dead NPC is skipped by every other phase, and because this
 * is what makes Cage-Keeper's Death Release and every on-death summon work
 * without a special case at each of the four places NPC health is written.
 *
 * Latched, so it fires once per death however many ticks the corpse persists.
 */
void npc_triggers_on_death(NPCThink* t);

/* --- Phases (§4.4.1) ------------------------------------------------------
 *
 * A phase is a named subset of a type's abilities plus an exit condition. A type
 * with no phases has one implicit phase containing everything, which is every
 * enemy but two -- so both functions below are cheap no-ops for almost every NPC.
 */

/** Report whether the NPC's current phase permits one of its ability slots.
 *
 * @return Nonzero when the type declares no phases, or when the active phase
 *         names this slot.
 */
int npc_phase_allows(NPCThink* t, int ability_slot);

/** Advance the phase machine, cycling in declaration order when the exit passes.
 *
 * Called once per tick with the slot lock held, after the act phase, so that an
 * `after_uses` exit sees this tick's use.
 */
void npc_phase_advance(NPCThink* t);

/** Record that this NPC took damage, for `incoming_damage` triggers.
 *
 * Called wherever NPC health is written, with that NPC's slot lock held. A
 * counter rather than a flag: two hits between two AI ticks are two events.
 *
 * @param from_x, from_y  Where the damage came from, for the reactive hop's direction.
 */
void npc_trigger_note_damage(NPCEntity* npc, float from_x, float from_y);

/** Record that this NPC killed a player, for `on_kill` triggers. */
void npc_trigger_note_kill(NPCEntity* npc);

#endif // NPC_TRIGGERS_H
