#ifndef NPC_WORLD_H
#define NPC_WORLD_H

/** @file Own the world's NPC pool, its per-entity locks, and its identifier index.
 *
 * The pool used to be `NPCEntity npcs[256]` behind a single mutex. Every phase of
 * every tick re-scanned all 256 slots under that one lock, and the projectile
 * phase re-took it once per projectile — so a busy tick serialised combat,
 * abilities, projectiles, AI, and every inbound packet against each other, and
 * 256 was a hard ceiling on world size that no tuning could move.
 *
 * This replaces both problems:
 *
 *  - The pool is heap-allocated at startup and sized from the world's .conf
 *    (`max_npcs`), so the ceiling is a deployment decision rather than a
 *    recompile. Storage never moves afterwards, which is what makes it safe to
 *    hand out NPCEntity pointers.
 *
 *  - Locking is split in two. A pool-wide rwlock guards slot allocation, slot
 *    release, and the identifier index — the only operations that change which
 *    slot holds what. Each slot then has its own mutex guarding that one NPC's
 *    fields. Readers share the pool lock; two threads damaging two different
 *    NPCs no longer contend at all.
 *
 * Lock order is pool-read then slot, always, and never two slots at once.
 * Everything in this header obeys that; callers must too.
 *
 * @see npc_snapshot.h for the per-tick spatial index built over this pool, which
 *      is how gameplay phases avoid scanning it at all.
 */

#include "combat_config.h"

#include <pthread.h>
#include <stdint.h>

/** Size the NPC pool when a world configuration does not say. */
#define NPC_CAPACITY_DEFAULT 256

/** Refuse absurd configured capacities rather than trying to allocate them. */
#define NPC_CAPACITY_MAX 65536

/** Hold the world's NPC storage, its per-slot locks, and its id index.
 *
 * Treat every field as private. The pool is only correct when reached through
 * the functions below, which is also the only way the lock order is enforced.
 */
typedef struct {
    NPCEntity*       npcs;        /**< capacity entries; the address never moves after init. */
    pthread_mutex_t* slot_locks;  /**< One mutex per slot, parallel to npcs. */

    int capacity;                 /**< Allocated slots; fixed for the pool's lifetime. */
    int count;                    /**< Occupied slots. Guarded by lock. */

    /** Guard slot ownership and the identifier index, not NPC field contents. */
    pthread_rwlock_t lock;

    /** Map identifier to slot by open addressing; -1 marks a free bucket.
     *
     * Sized to a power of two strictly greater than capacity so probing always
     * terminates on an empty bucket. Guarded by lock alongside slot ownership,
     * because an identifier and the slot holding it change together or not at all.
     */
    int32_t* index_slot;
    uint32_t index_mask;

    /** Occupied slots, unordered, `count` of them. Guarded by lock.
     *
     * A pool sized for the world's peak is mostly empty for most of its life,
     * and the phases that must consider every NPC used to walk `capacity`
     * slots to find them. At 20Hz against a pool of 4096 configured for a
     * launch-day crowd, that is 82,000 slot reads a second to think for the
     * two hundred NPCs actually spawned.
     *
     * This is the list of the ones that exist. It is written only where slot
     * ownership changes -- spawn and remove -- under the same write lock, so a
     * scan holding the read lock sees a list that matches the pool.
     */
    int* live_slots;
    /** Slot -> position in live_slots, or -1. Makes removal O(1). */
    int* live_position;
} NPCWorld;

/** Allocate a pool of the given capacity.
 *
 * @param world  Pool to initialize; must not already be initialized.
 * @param capacity  Requested slots; clamped to [1, NPC_CAPACITY_MAX].
 * @return  1 on success, or 0 when allocation fails.
 */
int npc_world_init(NPCWorld* world, int capacity);

/** Release every allocation owned by a pool and leave it zeroed. */
void npc_world_shutdown(NPCWorld* world);

/** Report the pool's fixed slot capacity. Safe without any lock. */
int npc_world_capacity(const NPCWorld* world);

/** Report how many slots are occupied, taking the pool read lock. */
int npc_world_count(NPCWorld* world);

/** Spawn an NPC into a free or reclaimable slot.
 *
 * @return The assigned identifier, or 0 when no slot is available.
 */
uint32_t npc_world_spawn(NPCWorld* world,
                         const char* name,
                         float x, float y,
                         int health,
                         float hitbox_radius,
                         uint32_t dialogue_id,
                         uint8_t is_interactable,
                         uint16_t npc_type_id,
                         float respawn_time,
                         uint8_t category);

/** Remove an NPC by identifier, freeing its slot and index entry. */
void npc_world_remove(NPCWorld* world, uint32_t npc_id);

/* --- Scanning -----------------------------------------------------------
 *
 * A scan holds the pool read lock across a loop over slots, which keeps slot
 * ownership stable, then takes and drops one slot lock at a time inside the
 * loop. Only phases that genuinely must consider every NPC — AI thinking,
 * respawn sweeps — should scan. Anything with a position and a radius should
 * query the tick snapshot instead.
 */

/** Take the pool read lock for a slot-by-slot scan. */
void npc_world_read_begin(NPCWorld* world);

/** Release the pool read lock taken by npc_world_read_begin(). */
void npc_world_read_end(NPCWorld* world);

/** Borrow the occupied-slot list for a scan.
 *
 * Valid only while the caller holds the pool read lock, and only until it is
 * released: spawn and remove rewrite the list. Iterating this instead of
 * `capacity` is what makes a scan cost the number of NPCs that exist rather
 * than the size of the pool they were given room in.
 *
 * @param out_count  Receives how many slots the list holds.
 * @return           The list, or NULL when the pool is empty or uninitialized.
 */
const int* npc_world_live_slots(const NPCWorld* world, int* out_count);

/** Return the entity in a slot, or NULL when out of range.
 *
 * Valid only inside a scan, and only for reading the identifier to decide
 * whether the slot is worth locking. Read or write any other field only with
 * the slot lock held.
 */
NPCEntity* npc_world_slot(NPCWorld* world, int slot);

/** Lock one slot during a scan. The caller must already hold the pool read lock. */
void npc_world_slot_lock(NPCWorld* world, int slot);

/** Unlock a slot locked by npc_world_slot_lock(). */
void npc_world_slot_unlock(NPCWorld* world, int slot);

/** Find where the nearest living NPC of a type currently stands.
 *
 * A scan, and deliberately so: this answers "where is the quest giver" for a
 * type that may be spawned once or a hundred times, and it runs when a quest
 * is accepted or advances rather than per tick.
 *
 * @param from_x, from_y  Position to measure from; the nearest match wins.
 * @param out_x, out_y    Receive the position when one is found.
 * @return                1 when a living NPC of that type exists, or 0 otherwise.
 */
int npc_world_nearest_of_type(NPCWorld* world, uint16_t npc_type_id,
                              float from_x, float from_y,
                              float* out_x, float* out_y);

/* --- Acquisition --------------------------------------------------------
 *
 * The single-NPC counterpart to a scan, mirroring player_acquire()/
 * player_release() in player_data.h. On success the caller holds the pool read
 * lock and one slot lock, and must release both by calling npc_world_release()
 * on every path out — including early returns.
 *
 * Do not hold two acquisitions at once. Nothing needs to, and the moment
 * something does, two threads acquiring the same pair in opposite orders
 * deadlock.
 */

/** Acquire an NPC by identifier for reading or mutation.
 *
 * @return The locked entity, or NULL when no slot holds that identifier.
 */
NPCEntity* npc_world_acquire(NPCWorld* world, uint32_t npc_id);

/** Acquire a known slot, confirming it still holds the expected identifier.
 *
 * The confirmation is the point: a slot named by a tick snapshot may have been
 * freed and reused by the time a later phase of the same tick reaches it, and
 * damage applied without this check would land on whatever spawned in its place.
 *
 * @return The locked entity, or NULL when the slot was recycled or is empty.
 */
NPCEntity* npc_world_acquire_slot(NPCWorld* world, int slot, uint32_t expected_id);

/** Release an entity acquired by npc_world_acquire() or npc_world_acquire_slot(). */
void npc_world_release(NPCWorld* world, NPCEntity* npc);

#endif // NPC_WORLD_H
