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

/* --- Per-NPC variable-length state ---------------------------------------
 *
 * Ability cooldowns, status effects and trigger latches are all per-NPC state
 * whose width is a property of the *content*, not of the code. They used to be
 * inline arrays on NPCEntity sized by a #define -- and ability cooldowns were
 * sized by a second #define in another header that a comment asked you to keep
 * in step by hand. Raising one and not the other was not a truncation; it was an
 * out-of-bounds write into the pool on the gameplay thread.
 *
 * They live here instead, as arrays parallel to `npcs`, allocated once at init
 * to the widths the loaded content actually needs and indexed [slot * stride + i].
 * Each is guarded by the slot mutex that already guards that NPC, so this adds
 * storage without adding a lock or touching the lock order.
 *
 * NPCEntity stays plain data, which is what keeps it memset- and copy-safe and
 * keeps the tick snapshot cheap -- the snapshot copies scalars and never this.
 */

/** Fall back to these when a caller does not size state from loaded content. */
#define NPC_STATE_ABILITIES_DEFAULT     4
#define NPC_STATE_EFFECT_SLOTS_DEFAULT  8

/** Refuse absurd configured widths rather than allocating them. */
#define NPC_STATE_ABILITIES_MAX     64
#define NPC_STATE_EFFECT_SLOTS_MAX  64

/** Track one status effect on an NPC.
 *
 * Deliberately field-for-field the player's active_effects slot in
 * server_types.h: the two are the same mechanic, and npc_effects.c is meant to
 * mirror player_effects.c closely enough that a reader of one can read the other.
 */
typedef struct {
    uint8_t  active;
    uint8_t  effect_type;      /**< StatusEffectType. */
    uint8_t  buff_stat;
    uint8_t  _pad;
    int      value;
    float    duration_remaining;
    float    tick_remaining;
    float    tick_rate;
    uint32_t source_id;
} NPCEffect;

/** Size the per-NPC state arrays from what the content actually needs.
 *
 * Every field may be 0, which takes the compiled default. Startup fills this from
 * npc_registry_max_abilities() and friends, so a shard whose widest enemy has
 * three abilities allocates three -- and a seven-ability mini-boss needs no code
 * change, only a registry that reports seven.
 */
typedef struct {
    int max_abilities;     /**< Cooldown slots per NPC. */
    int max_effect_slots;  /**< Status effect slots per NPC. */
    int max_triggers;      /**< Trigger latches per NPC; stored as a bitset. */
    int max_phases;        /**< Phases per NPC; 0 when no type declares any. */
} NPCStateSizes;

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

    /* --- Per-NPC variable-length state, indexed [slot * stride + i]. ------
     *
     * Guarded by that slot's mutex, like the NPCEntity it belongs to. Cleared
     * when a slot is claimed and when it is released, so a recycled slot never
     * inherits the previous occupant's cooldowns, buffs or fired latches. */

    double*    ability_cooldowns;  /**< capacity * max_abilities. */
    /** Per-slot ability redirection, capacity * max_abilities.
     *
     * Zero means "use the ability this type declares in that slot"; anything else
     * is a registry ability index biased by one, written by a fired
     * `swap_ability` trigger. Alpha Wolf's Double Bite swaps its lunge for the
     * two-shot `serial_burst`, which is not in its own ability list, so the
     * target has to be a registry index rather than a type-local slot.
     *
     * A redirection table rather than a mutated ability list, because the list
     * belongs to the registry and the registry is shared by every instance of a
     * type -- one Alpha Wolf reaching 40% health must not rearm every other. */
    uint16_t*  ability_swap;
    NPCEffect* effects;            /**< capacity * max_effect_slots. */
    uint32_t*  trigger_latches;    /**< capacity * latch_words; one bit per trigger. */
    uint8_t*   phase_index;        /**< capacity; current phase. */
    double*    phase_started_at;   /**< capacity; for `after` phase exits. */
    uint16_t*  phase_uses;         /**< capacity; for `after_uses` phase exits. */

    int max_abilities;
    int max_effect_slots;
    int max_triggers;
    int latch_words;               /**< ceil(max_triggers / 32); 0 when no triggers. */
    int max_phases;
} NPCWorld;

/** Allocate a pool of the given capacity.
 *
 * @param world  Pool to initialize; must not already be initialized.
 * @param capacity  Requested slots; clamped to [1, NPC_CAPACITY_MAX].
 * @return  1 on success, or 0 when allocation fails.
 */
int npc_world_init(NPCWorld* world, int capacity);

/** Allocate a pool whose per-NPC state is sized from the loaded content.
 *
 * The sizing counterpart to npc_world_init(), which is this with `sizes` NULL.
 * Prefer this at startup: passing npc_registry_max_abilities() here is what makes
 * the widest enemy in the data file a fact the pool is built around, rather than
 * a number two headers have to agree on.
 *
 * @param world     Pool to initialize; must not already be initialized.
 * @param capacity  Requested slots; clamped to [1, NPC_CAPACITY_MAX].
 * @param sizes     Per-NPC state widths; NULL or zeroed fields take the defaults.
 * @return          1 on success, or 0 when allocation fails.
 */
int npc_world_init_sized(NPCWorld* world, int capacity, const NPCStateSizes* sizes);

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

/* --- Per-NPC state ------------------------------------------------------
 *
 * Each returns that slot's span of the parallel arrays, or NULL when the slot is
 * out of range or the pool holds no state of that kind. The caller must already
 * hold the slot's lock -- the same lock that guards the NPCEntity itself.
 */

/** Borrow a slot's ability cooldowns. npc_world_max_abilities() entries. */
double* npc_world_cooldowns(NPCWorld* world, int slot);

/** Read a slot's ability redirection.
 *
 * @return The registry ability index now standing in for that type-local slot,
 *         or -1 when the slot still uses the ability its type declares.
 */
int npc_world_ability_override(NPCWorld* world, int slot, int ability);

/** Redirect one of a slot's type-local abilities to a registry ability.
 *
 * @param registry_index  Registry ability index, or -1 to clear the redirection.
 */
void npc_world_ability_swap(NPCWorld* world, int slot, int ability,
                            int registry_index);

/** Borrow a slot's status effect array. npc_world_max_effect_slots() entries. */
NPCEffect* npc_world_effects(NPCWorld* world, int slot);

/** Borrow a slot's trigger latch bitset. npc_world_latch_words() words. */
uint32_t* npc_world_latches(NPCWorld* world, int slot);

/** Report whether a trigger's latch has fired for one NPC. */
int npc_world_latch_get(NPCWorld* world, int slot, int trigger);

/** Set a trigger's latch for one NPC. Ignores an out-of-range trigger. */
void npc_world_latch_set(NPCWorld* world, int slot, int trigger);

/** Clear every piece of per-NPC state for a slot.
 *
 * Called when a slot is claimed and when it is released. Also what a respawn
 * wants: a returning NPC should not resume with the cooldowns it died holding.
 */
void npc_world_clear_state(NPCWorld* world, int slot);

int npc_world_max_abilities(const NPCWorld* world);
int npc_world_max_effect_slots(const NPCWorld* world);
int npc_world_latch_words(const NPCWorld* world);

#endif // NPC_WORLD_H
