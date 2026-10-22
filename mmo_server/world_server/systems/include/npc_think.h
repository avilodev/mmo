#ifndef NPC_THINK_H
#define NPC_THINK_H

/** @file Carry everything one NPC's think phase needs, so nothing re-derives it.
 *
 * The think phase is spread over three files -- triggers, behaviour, and the tick
 * that orchestrates them -- and all three need the same eight things: the pool and
 * slot they hold the lock on, the entity, its profile, this tick's two snapshots,
 * the clock, and the queue to append after-the-locks work to.
 *
 * Passing them as one struct rather than as eight parameters is not only brevity.
 * The target fields are the reason: target selection happens once, in the
 * behaviour phase, and both the trigger evaluator and the act phase need its
 * result. Recomputing it in each would let two parts of one tick disagree about
 * who this NPC is fighting.
 */

#include "npc_ai.h"
#include "npc_deferred.h"
#include "npc_snapshot.h"
#include "npc_world.h"
#include "tick_snapshot.h"

/** Hold one NPC's per-tick working set. Valid only while its slot lock is held. */
typedef struct {
    NPCWorld*           world;
    int                 slot;
    NPCEntity*          npc;
    const NPCAIProfile* prof;

    TickSnapshot*    players;
    NPCTickSnapshot* npcs;      /**< May be NULL on a tick with no NPC snapshot. */

    double now;
    float  dt;

    NPCDeferredQueue* q;

    /** This NPC's cooldown array and its width, taken before the slot lock.
     *
     * The base pointer is fixed for the pool's lifetime, so caching it is safe;
     * its contents are guarded by the slot lock like any other NPC field.
     */
    double* cooldowns;
    int     cooldown_count;

    /** Target resolved by the behaviour phase this tick. */
    int      has_target;
    int      target_dense;      /**< Index into `players`; -1 when has_target is 0. */
    uint32_t target_id;
    float    target_x, target_y;
    float    target_dist;

    /** This NPC's faction, resolved once. -1 for a type with no faction. */
    int faction_index;

    /** Set by the act phase when it starts or completes an ability, so the phase
     * machine can count uses without the act phase knowing phases exist. */
    int ability_used;
} NPCThink;

#endif // NPC_THINK_H
