#ifndef NPC_SUMMON_H
#define NPC_SUMMON_H

/** @file Turn a registry type into a live NPC, at load time or mid-fight (V7).
 *
 * `npc_world_spawn()` takes a name, a position and a handful of scalars, because
 * it was written for a spawn file that carried all of them. A summon has none of
 * them: it has a type, and everything else -- name, health, armour, hitbox, XP,
 * behaviour -- is what that type *is*. This module is the single place a registry
 * type becomes an entity, so a Skeleton summoned by a Grave Caller and a Skeleton
 * placed by a spawn table are the same enemy rather than two that drifted.
 *
 * Three things a summon needs that a placed NPC does not:
 *
 *  - **Parentage.** A summon whose summoner dies goes with it, which is what
 *    makes killing the Packmaster the answer to the pack rather than a detail.
 *  - **A budget.** Without one, a summoner with a two-second cooldown fills the
 *    pool, and every other NPC in the world stops spawning.
 *  - **No reward.** Summons grant no XP and roll no loot. A summoner that pays
 *    out is an infinite XP faucet, and it is the first thing a player finds.
 */

#include "npc_snapshot.h"
#include "npc_world.h"

#include <stdint.h>

/** Cap live summons per parent when a world configuration does not say. */
#define SUMMON_PER_PARENT_DEFAULT 6

/** Refuse an absurd configured per-parent cap. */
#define SUMMON_PER_PARENT_MAX 64

/** Configure the summon budget.
 *
 * @param per_parent  Live summons one NPC may own; 0 takes the default.
 */
void npc_summon_configure(int per_parent);

/** Report the configured per-parent budget. */
int npc_summon_per_parent(void);

/** Spawn one NPC of a registry type.
 *
 * Every field but the position comes from the type: this is the whole point of
 * the function existing. The caller must hold no NPC lock -- spawning takes the
 * pool write lock.
 *
 * @param type_index    Index into the registry's type table.
 * @param parent_id     Summoner's identifier, or 0 for a placed NPC.
 * @param affix_index   Registry affix to compose on, or -1 for none.
 * @return              The new NPC's identifier, or 0 on failure.
 */
uint32_t npc_spawn_from_type(NPCWorld* world, int type_index,
                             float x, float y, uint32_t parent_id,
                             int affix_index);

/** Spawn a summon group, respecting the parent's budget.
 *
 * @param type_index  Registry type indices to draw from, chosen round-robin.
 * @param type_count  How many entries `type_index` holds.
 * @param count       How many to spawn, before the budget is applied.
 * @param spread      World-unit radius the group is scattered within.
 * @return            How many actually spawned.
 */
int npc_summon_group(NPCWorld* world, uint32_t parent_id,
                     float x, float y,
                     const int* type_index, int type_count,
                     int count, float spread);

/** Despawn dead and orphaned summons, and recount every summoner's budget.
 *
 * Called once per tick from the AI tick, with no lock held: it takes the pool
 * write lock to remove, which is impossible from inside a scan.
 *
 * The recount is what keeps the budget honest. Incrementing on summon and
 * decrementing on death is the shape that drifts -- one missed path and a
 * summoner is permanently at its cap, which reads in game as a boss that stops
 * summoning halfway through the fight and cannot be told from intended
 * behaviour. Counting what is actually alive cannot drift.
 *
 * @param children     Summon identifiers, collected during the scan.
 * @param parents      Their summoners' identifiers, parallel to `children`.
 * @param alive        Whether each was alive, parallel to `children`.
 * @param count        How many entries the arrays hold.
 * @return             How many summons were despawned.
 */
int npc_summon_reap(NPCWorld* world, const uint32_t* children,
                    const uint32_t* parents, const uint8_t* alive, int count);

#endif // NPC_SUMMON_H
