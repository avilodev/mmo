#ifndef NPC_SNAPSHOT_H
#define NPC_SNAPSHOT_H

/** @file Snapshot living NPCs once per gameplay tick and index them spatially.
 *
 * The NPC counterpart to tick_snapshot.h, and it exists for the same reason.
 * Before it, every gameplay phase re-scanned the whole NPC pool under one lock:
 * target selection did it inside a per-player loop, and the projectile phase did
 * it once per projectile, re-taking the pool lock each time. At 128 projectiles
 * and 256 NPCs that alone was 32,768 distance checks and 128 lock round-trips
 * per tick, and none of it got cheaper as the pool grew.
 *
 * The gameplay thread now samples the pool once, at the top of the tick, into
 * dense arrays with a spatial grid over them. Every phase queries that grid
 * instead of scanning, so the cost tracks how many NPCs are near the caster
 * rather than how many exist.
 *
 * The snapshot is a read-only index, never a substitute for the pool. Its values
 * are a copy taken at the top of the tick, so it is the right thing to reject
 * candidates with — a distance test, a cone test — and the wrong thing to apply
 * damage to. Resolve a survivor through npc_world_acquire_slot() using the
 * slot[] and id[] this records; that is what re-checks the NPC is still there
 * and still the same one.
 *
 * Owned by the single gameplay thread, exactly like TickSnapshot: the grid keeps
 * query scratch inside itself, so it must not be queried from two threads.
 */

#include "npc_world.h"
#include "spatial_grid.h"

#include <stdint.h>

/** Pair dense living-NPC arrays with a pass-local spatial index.
 *
 * All arrays are parallel and hold `count` entries. Dead and empty slots are
 * excluded at build time, so a dense index always names a living NPC as of the
 * top of this tick.
 */
typedef struct {
    int capacity;   /**< Pool capacity this snapshot was sized against. */
    int count;      /**< Living NPCs sampled this tick. */

    int*      slot;           /**< Pool slot, for re-acquiring the real entity. */
    uint32_t* id;
    float*    pos_x;
    float*    pos_y;
    float*    hitbox_radius;
    int32_t*  health;
    int32_t*  max_health;
    int32_t*  armor;
    uint32_t* xp_reward;
    uint16_t* npc_type_id;
    uint8_t*  category;

    /** Map pool slots to dense indices, using -1 when absent. */
    int* dense_of_slot;

    /** Largest hitbox radius in this snapshot.
     *
     * Query radii have to be widened by this: a shape test is against an NPC's
     * edge, but the grid indexes its centre, so a search of exactly `range`
     * would miss a large NPC whose body is in range while its centre is not.
     */
    float max_hitbox_radius;

    SpatialGrid*  grid;
    SpatialPoint* points;     /**< Scratch used during grid rebuilds. */
} NPCTickSnapshot;

/** Allocate a snapshot sized for a pool. Returns 0 on failure. */
int npc_snapshot_init(NPCTickSnapshot* snapshot, const NPCWorld* world);

/** Release a snapshot's storage and grid. */
void npc_snapshot_free(NPCTickSnapshot* snapshot);

/** Resample living NPCs and rebuild the grid, briefly holding the pool locks. */
void npc_snapshot_build(NPCTickSnapshot* snapshot, NPCWorld* world);

/** Write nearest-first dense indices within a radius; returns at most max_out. */
int npc_snapshot_query(NPCTickSnapshot* snapshot, float x, float y, float radius,
                       int* out_indices, int max_out);

#endif // NPC_SNAPSHOT_H
