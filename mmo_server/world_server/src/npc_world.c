/**
 * @file
 * Allocate, lock, and index the world's NPC pool.
 */

#include "npc_world.h"

#include "log.h"
#include "str_fixed.h"

#include <stdlib.h>
#include <string.h>

/** Assign identifiers from a single counter, never reusing one.
 *
 * Slots are reused; identifiers are not. That is what lets a tick snapshot name
 * a slot and have a later phase detect that the slot has changed hands.
 */
static uint32_t g_next_npc_id = 1000;

/** Mark an index bucket that has never held an entry. */
#define INDEX_EMPTY (-1)

/** Mark an index bucket whose entry was removed, so probing continues past it. */
#define INDEX_TOMBSTONE (-2)

/** Spread sequential identifiers across index buckets. */
static uint32_t index_hash(uint32_t id) {
    // Knuth multiplicative hash. Identifiers are handed out sequentially, so
    // masking the raw value would file every NPC in one contiguous run.
    return id * 2654435761u;
}

/** Round up to a power of two strictly greater than a value. */
static uint32_t index_capacity_for(int capacity) {
    uint32_t buckets = 1;
    while (buckets <= (uint32_t)capacity * 2u) buckets <<= 1;
    return buckets;
}

/** Record an identifier's slot. The caller must hold the pool write lock. */
static void index_insert(NPCWorld* world, uint32_t id, int slot) {
    uint32_t bucket = index_hash(id) & world->index_mask;

    for (uint32_t probe = 0; probe <= world->index_mask; probe++) {
        int32_t occupant = world->index_slot[bucket];
        if (occupant == INDEX_EMPTY || occupant == INDEX_TOMBSTONE ||
            world->npcs[occupant].id == id) {
            world->index_slot[bucket] = slot;
            return;
        }
        bucket = (bucket + 1) & world->index_mask;
    }

    // Unreachable: buckets exceed capacity, so a free one always exists.
    LOG_ERROR("[NPC] identifier index is full at %u buckets", world->index_mask + 1);
}

/** Resolve an identifier to its slot, or -1. The caller must hold the pool lock. */
static int index_find(NPCWorld* world, uint32_t id) {
    uint32_t bucket = index_hash(id) & world->index_mask;

    for (uint32_t probe = 0; probe <= world->index_mask; probe++) {
        int32_t occupant = world->index_slot[bucket];
        if (occupant == INDEX_EMPTY) return -1;
        if (occupant != INDEX_TOMBSTONE && world->npcs[occupant].id == id)
            return occupant;
        bucket = (bucket + 1) & world->index_mask;
    }
    return -1;
}

/** Drop an identifier's index entry. The caller must hold the pool write lock. */
static void index_erase(NPCWorld* world, uint32_t id) {
    uint32_t bucket = index_hash(id) & world->index_mask;

    for (uint32_t probe = 0; probe <= world->index_mask; probe++) {
        int32_t occupant = world->index_slot[bucket];
        if (occupant == INDEX_EMPTY) return;
        if (occupant != INDEX_TOMBSTONE && world->npcs[occupant].id == id) {
            // Tombstone rather than empty: an entry that probed past this
            // bucket would become unreachable if the chain were broken here.
            world->index_slot[bucket] = INDEX_TOMBSTONE;
            return;
        }
        bucket = (bucket + 1) & world->index_mask;
    }
}

/**
 * Allocate a pool of the given capacity.
 *
 * @param world  Pool to initialize.
 * @param capacity  Requested slots, clamped to [1, NPC_CAPACITY_MAX].
 * @return  1 on success, or 0 for NULL input or allocation failure.
 */
/** Clamp one configured state width to its ceiling, taking the default at zero. */
static int state_width(int requested, int fallback, int ceiling, const char* what) {
    if (requested <= 0) return fallback;
    if (requested > ceiling) {
        LOG_ERROR("[NPC] %s of %d is past the %d this build allocates; keeping %d",
                  what, requested, ceiling, ceiling);
        return ceiling;
    }
    return requested;
}

int npc_world_init(NPCWorld* world, int capacity) {
    return npc_world_init_sized(world, capacity, NULL);
}

int npc_world_init_sized(NPCWorld* world, int capacity, const NPCStateSizes* sizes) {
    if (!world) return 0;

    if (capacity <= 0)                capacity = NPC_CAPACITY_DEFAULT;
    if (capacity > NPC_CAPACITY_MAX)  capacity = NPC_CAPACITY_MAX;

    memset(world, 0, sizeof(*world));
    world->capacity = capacity;

    uint32_t buckets = index_capacity_for(capacity);
    world->index_mask = buckets - 1;

    world->npcs          = calloc((size_t)capacity, sizeof(NPCEntity));
    world->slot_locks    = calloc((size_t)capacity, sizeof(pthread_mutex_t));
    world->index_slot    = calloc((size_t)buckets, sizeof(int32_t));
    world->live_slots    = calloc((size_t)capacity, sizeof(int));
    world->live_position = calloc((size_t)capacity, sizeof(int));

    /* Per-NPC state, sized from the content rather than from a #define. */
    world->max_abilities = state_width(sizes ? sizes->max_abilities : 0,
                                       NPC_STATE_ABILITIES_DEFAULT,
                                       NPC_STATE_ABILITIES_MAX, "ability width");
    world->max_effect_slots = state_width(sizes ? sizes->max_effect_slots : 0,
                                          NPC_STATE_EFFECT_SLOTS_DEFAULT,
                                          NPC_STATE_EFFECT_SLOTS_MAX, "effect slots");
    world->max_triggers = sizes && sizes->max_triggers > 0 ? sizes->max_triggers : 0;
    world->latch_words  = (world->max_triggers + 31) / 32;
    world->max_phases   = sizes && sizes->max_phases > 0 ? sizes->max_phases : 0;

    world->ability_cooldowns = calloc((size_t)capacity * (size_t)world->max_abilities,
                                      sizeof(double));
    world->ability_swap = calloc((size_t)capacity * (size_t)world->max_abilities,
                                 sizeof(uint16_t));
    world->effects = calloc((size_t)capacity * (size_t)world->max_effect_slots,
                            sizeof(NPCEffect));
    world->trigger_latches = world->latch_words
        ? calloc((size_t)capacity * (size_t)world->latch_words, sizeof(uint32_t))
        : NULL;
    world->phase_index      = calloc((size_t)capacity, sizeof(uint8_t));
    world->phase_started_at = calloc((size_t)capacity, sizeof(double));
    world->phase_uses       = calloc((size_t)capacity, sizeof(uint16_t));

    if (!world->npcs || !world->slot_locks || !world->index_slot ||
        !world->live_slots || !world->live_position ||
        !world->ability_cooldowns || !world->ability_swap || !world->effects ||
        (world->latch_words && !world->trigger_latches) ||
        !world->phase_index || !world->phase_started_at || !world->phase_uses) {
        LOG_ERROR("[NPC] could not allocate a pool of %d NPCs", capacity);
        npc_world_shutdown(world);
        return 0;
    }

    for (int i = 0; i < capacity; i++) {
        pthread_mutex_init(&world->slot_locks[i], NULL);
        world->live_position[i] = -1;   /* zero would read as "listed first" */
    }

    for (uint32_t b = 0; b < buckets; b++)
        world->index_slot[b] = INDEX_EMPTY;

    pthread_rwlock_init(&world->lock, NULL);

    size_t state_bytes = (size_t)capacity *
        ((size_t)world->max_abilities * sizeof(double) +
         (size_t)world->max_effect_slots * sizeof(NPCEffect) +
         (size_t)world->latch_words * sizeof(uint32_t) +
         sizeof(uint8_t) + sizeof(double) + sizeof(uint16_t));

    LOG_INFO("[NPC] pool initialized: %d slots, %u index buckets, "
             "%d ability + %d effect slots each (%zu KB total)",
             capacity, buckets, world->max_abilities, world->max_effect_slots,
             ((size_t)capacity * (sizeof(NPCEntity) + sizeof(pthread_mutex_t)) +
              (size_t)buckets * sizeof(int32_t) + state_bytes) / 1024);
    return 1;
}

/**
 * Release every allocation owned by a pool and leave it zeroed.
 */
void npc_world_shutdown(NPCWorld* world) {
    if (!world) return;

    if (world->slot_locks) {
        for (int i = 0; i < world->capacity; i++)
            pthread_mutex_destroy(&world->slot_locks[i]);
    }
    if (world->npcs) pthread_rwlock_destroy(&world->lock);

    free(world->npcs);
    free(world->slot_locks);
    free(world->index_slot);
    free(world->live_slots);
    free(world->live_position);
    free(world->ability_cooldowns);
    free(world->ability_swap);
    free(world->effects);
    free(world->trigger_latches);
    free(world->phase_index);
    free(world->phase_started_at);
    free(world->phase_uses);
    memset(world, 0, sizeof(*world));
}

/* --- Per-NPC state ------------------------------------------------------- */

/** Report whether a slot index names a real slot in an initialized pool. */
static int slot_ok(const NPCWorld* world, int slot) {
    return world && world->npcs && slot >= 0 && slot < world->capacity;
}

double* npc_world_cooldowns(NPCWorld* world, int slot) {
    if (!slot_ok(world, slot) || !world->ability_cooldowns) return NULL;
    return &world->ability_cooldowns[(size_t)slot * (size_t)world->max_abilities];
}

int npc_world_ability_override(NPCWorld* world, int slot, int ability) {
    if (!slot_ok(world, slot) || !world->ability_swap) return -1;
    if (ability < 0 || ability >= world->max_abilities) return -1;
    uint16_t to = world->ability_swap[(size_t)slot * (size_t)world->max_abilities +
                                      (size_t)ability];
    /* Zero is the cleared state, which means "not redirected" rather than
     * "redirected to registry ability 0" -- npc_world_clear_state() memsets this
     * array and a respawned NPC must come back with the kit its type declares.
     * Redirections are therefore stored biased by one. */
    return to == 0 ? -1 : (int)to - 1;
}

void npc_world_ability_swap(NPCWorld* world, int slot, int ability,
                            int registry_index) {
    if (!slot_ok(world, slot) || !world->ability_swap) return;
    if (ability < 0 || ability >= world->max_abilities) return;
    if (registry_index < -1 || registry_index >= 0xFFFE) return;
    world->ability_swap[(size_t)slot * (size_t)world->max_abilities + (size_t)ability] =
        registry_index < 0 ? 0 : (uint16_t)(registry_index + 1);
}

NPCEffect* npc_world_effects(NPCWorld* world, int slot) {
    if (!slot_ok(world, slot) || !world->effects) return NULL;
    return &world->effects[(size_t)slot * (size_t)world->max_effect_slots];
}

uint32_t* npc_world_latches(NPCWorld* world, int slot) {
    if (!slot_ok(world, slot) || !world->trigger_latches) return NULL;
    return &world->trigger_latches[(size_t)slot * (size_t)world->latch_words];
}

int npc_world_latch_get(NPCWorld* world, int slot, int trigger) {
    if (trigger < 0 || !world || trigger >= world->max_triggers) return 0;
    uint32_t* words = npc_world_latches(world, slot);
    if (!words) return 0;
    return (words[trigger / 32] >> (trigger % 32)) & 1u;
}

void npc_world_latch_set(NPCWorld* world, int slot, int trigger) {
    if (trigger < 0 || !world || trigger >= world->max_triggers) return;
    uint32_t* words = npc_world_latches(world, slot);
    if (!words) return;
    words[trigger / 32] |= 1u << (trigger % 32);
}

void npc_world_clear_state(NPCWorld* world, int slot) {
    if (!slot_ok(world, slot)) return;

    double* cds = npc_world_cooldowns(world, slot);
    if (cds) memset(cds, 0, (size_t)world->max_abilities * sizeof(*cds));

    if (world->ability_swap)
        memset(&world->ability_swap[(size_t)slot * (size_t)world->max_abilities], 0,
               (size_t)world->max_abilities * sizeof(uint16_t));

    NPCEffect* fx = npc_world_effects(world, slot);
    if (fx) memset(fx, 0, (size_t)world->max_effect_slots * sizeof(*fx));

    uint32_t* latches = npc_world_latches(world, slot);
    if (latches) memset(latches, 0, (size_t)world->latch_words * sizeof(*latches));

    if (world->phase_index)      world->phase_index[slot] = 0;
    if (world->phase_started_at) world->phase_started_at[slot] = 0.0;
    if (world->phase_uses)       world->phase_uses[slot] = 0;
}

int npc_world_max_abilities(const NPCWorld* world) {
    return world ? world->max_abilities : 0;
}
int npc_world_max_effect_slots(const NPCWorld* world) {
    return world ? world->max_effect_slots : 0;
}
int npc_world_latch_words(const NPCWorld* world) {
    return world ? world->latch_words : 0;
}

/**
 * Report the pool's fixed slot capacity.
 *
 * @return Allocated slots, or 0 for a NULL pool.
 */
int npc_world_capacity(const NPCWorld* world) {
    return world ? world->capacity : 0;
}

/**
 * Report how many slots are occupied.
 *
 * This function acquires the pool read lock.
 *
 * @return Occupied slots, or 0 for a NULL pool.
 */
int npc_world_count(NPCWorld* world) {
    if (!world) return 0;
    pthread_rwlock_rdlock(&world->lock);
    int count = world->count;
    pthread_rwlock_unlock(&world->lock);
    return count;
}

/**
 * Spawn an NPC into a free or reclaimable slot.
 *
 * This function acquires the pool write lock.
 *
 * @param world            Pool receiving the entity.
 * @param name             Terminated NPC display name.
 * @param x                Spawn X coordinate in world units.
 * @param y                Spawn Y coordinate in world units.
 * @param health           Initial and maximum health.
 * @param hitbox_radius    Collision radius in world units.
 * @param dialogue_id      Associated dialogue identifier, or zero.
 * @param is_interactable  Nonzero when client interaction is allowed.
 * @param npc_type_id      Content type used for AI, loot, and quests.
 * @param respawn_time     Respawn delay in seconds; nonpositive disables respawn.
 * @param category         NPC_CATEGORY_* value controlling default rewards.
 * @return                 The assigned identifier, or 0 when the pool is full.
 */
/* --- The occupied-slot list ---------------------------------------------- *
 *
 * An unordered array of occupied slot numbers plus a slot -> position map, so
 * both listing and removal are O(1) and neither ever walks the pool. Both are
 * written only under the pool write lock, alongside the slot ownership change
 * that made them true.
 */

/** Add a slot to the live list. Caller holds the pool write lock. */
static void live_list_add(NPCWorld* world, int slot) {
    if (world->live_position[slot] >= 0) return;   /* already listed */
    world->live_position[slot] = world->count;
    world->live_slots[world->count] = slot;
}

/** Remove a slot from the live list. Caller holds the pool write lock.
 *
 * The last entry is moved into the hole, which is why the list is unordered:
 * order costs a memmove per removal and nothing reads it in order.
 */
static void live_list_remove(NPCWorld* world, int slot) {
    int pos = world->live_position[slot];
    if (pos < 0) return;

    int last = world->count - 1;
    if (pos != last) {
        int moved = world->live_slots[last];
        world->live_slots[pos] = moved;
        world->live_position[moved] = pos;
    }
    world->live_position[slot] = -1;
}

const int* npc_world_live_slots(const NPCWorld* world, int* out_count) {
    if (out_count) *out_count = 0;
    if (!world || !world->live_slots || world->count <= 0) return NULL;
    if (out_count) *out_count = world->count;
    return world->live_slots;
}

uint32_t npc_world_spawn(NPCWorld* world,
                         const char* name,
                         float x, float y,
                         int health,
                         float hitbox_radius,
                         uint32_t dialogue_id,
                         uint8_t is_interactable,
                         uint16_t npc_type_id,
                         float respawn_time,
                         uint8_t category) {
    if (!world || !world->npcs) return 0;

    pthread_rwlock_wrlock(&world->lock);

    // Prefer a never-used slot, then fall back to reclaiming a dead one.
    int slot = -1;
    for (int i = 0; i < world->capacity; i++) {
        if (world->npcs[i].id == 0) { slot = i; break; }
    }
    if (slot == -1) {
        for (int i = 0; i < world->capacity; i++) {
            if (!world->npcs[i].is_alive) { slot = i; break; }
        }
    }
    if (slot == -1) {
        pthread_rwlock_unlock(&world->lock);
        LOG_ERROR("[NPC] pool full (%d/%d) — no reclaimable slot",
                  world->count, world->capacity);
        return 0;
    }

    NPCEntity* npc = &world->npcs[slot];

    // The write lock keeps other threads out of slot ownership, but a reader
    // that acquired this slot before it died may still hold its mutex.
    pthread_mutex_lock(&world->slot_locks[slot]);

    uint32_t previous_id = npc->id;
    if (previous_id != 0) index_erase(world, previous_id);

    memset(npc, 0, sizeof(*npc));
    /* A recycled slot must not inherit the previous occupant's cooldowns, buffs
     * or fired latches -- the state lives beside the entity now, not inside it,
     * so clearing the entity is no longer enough to clear the NPC. */
    npc_world_clear_state(world, slot);
    npc->id              = g_next_npc_id++;
    STR_COPY_FIELD(npc->name, name);
    npc->pos_x           = x;
    npc->pos_y           = y;
    npc->health          = health;
    npc->max_health      = health;
    npc->hitbox_radius   = hitbox_radius;
    npc->is_alive        = 1;
    npc->category        = category;
    npc->xp_reward       = (category == NPC_CATEGORY_HOSTILE) ? 50 : 0;
    npc->armor           = 0;
    npc->dialogue_id     = dialogue_id;
    npc->is_interactable = is_interactable;
    npc->spawn_x         = x;
    npc->spawn_y         = y;
    npc->npc_type_id     = npc_type_id;
    npc->respawn_time    = respawn_time;
    npc->death_time      = 0.0;

    uint32_t id = npc->id;
    index_insert(world, id, slot);

    /* The list is appended at the current count, so the count moves last. A
     * reclaimed slot is already listed and keeps its position. */
    if (previous_id == 0 && world->count < world->capacity) {
        live_list_add(world, slot);
        world->count++;
    }

    pthread_mutex_unlock(&world->slot_locks[slot]);
    pthread_rwlock_unlock(&world->lock);

    static const char* const category_names[] = {"passive", "hostile", "quest"};
    LOG_DEBUG("[NPC] spawned '%s' id=%u slot=%d at (%.1f, %.1f) hp=%d category=%s",
              name, id, slot, x, y, health,
              category_names[category < 3 ? category : 0]);
    return id;
}

/**
 * Remove an NPC by identifier, freeing its slot and index entry.
 *
 * This function acquires the pool write lock.
 */
void npc_world_remove(NPCWorld* world, uint32_t npc_id) {
    if (!world || !world->npcs || npc_id == 0) return;

    pthread_rwlock_wrlock(&world->lock);

    int slot = index_find(world, npc_id);
    if (slot >= 0) {
        pthread_mutex_lock(&world->slot_locks[slot]);
        index_erase(world, npc_id);
        memset(&world->npcs[slot], 0, sizeof(NPCEntity));
        npc_world_clear_state(world, slot);
        pthread_mutex_unlock(&world->slot_locks[slot]);

        /* Delisted before the count falls: live_list_remove() reads
         * world->count to find the last entry to swap in. */
        if (world->count > 0) {
            live_list_remove(world, slot);
            world->count--;
        }
        LOG_DEBUG("[NPC] removed id=%u from slot %d", npc_id, slot);
    }

    pthread_rwlock_unlock(&world->lock);
}

/**
 * Take the pool read lock for a slot-by-slot scan.
 */
void npc_world_read_begin(NPCWorld* world) {
    if (world && world->npcs) pthread_rwlock_rdlock(&world->lock);
}

/**
 * Release the pool read lock taken by npc_world_read_begin().
 */
void npc_world_read_end(NPCWorld* world) {
    if (world && world->npcs) pthread_rwlock_unlock(&world->lock);
}

/**
 * Return the entity in a slot without locking it.
 *
 * @return The slot's entity, or NULL when the slot is out of range.
 */
NPCEntity* npc_world_slot(NPCWorld* world, int slot) {
    if (!world || !world->npcs) return NULL;
    if (slot < 0 || slot >= world->capacity) return NULL;
    return &world->npcs[slot];
}

/**
 * Lock one slot during a scan.
 */
void npc_world_slot_lock(NPCWorld* world, int slot) {
    if (!world || !world->slot_locks) return;
    if (slot < 0 || slot >= world->capacity) return;
    pthread_mutex_lock(&world->slot_locks[slot]);
}

/**
 * Unlock a slot locked by npc_world_slot_lock().
 */
void npc_world_slot_unlock(NPCWorld* world, int slot) {
    if (!world || !world->slot_locks) return;
    if (slot < 0 || slot >= world->capacity) return;
    pthread_mutex_unlock(&world->slot_locks[slot]);
}

/**
 * Acquire an NPC by identifier for reading or mutation.
 *
 * On success the caller holds the pool read lock and the entity's slot lock,
 * and must release both with npc_world_release().
 *
 * @return The locked entity, or NULL when no slot holds that identifier.
 */
NPCEntity* npc_world_acquire(NPCWorld* world, uint32_t npc_id) {
    if (!world || !world->npcs || npc_id == 0) return NULL;

    pthread_rwlock_rdlock(&world->lock);

    int slot = index_find(world, npc_id);
    if (slot < 0) {
        pthread_rwlock_unlock(&world->lock);
        return NULL;
    }

    pthread_mutex_lock(&world->slot_locks[slot]);

    // The index was read under the read lock, so the slot cannot have been
    // reassigned — reassignment needs the write lock. Confirm anyway; the cost
    // is one comparison and it makes the invariant checkable rather than assumed.
    if (world->npcs[slot].id != npc_id) {
        pthread_mutex_unlock(&world->slot_locks[slot]);
        pthread_rwlock_unlock(&world->lock);
        return NULL;
    }

    return &world->npcs[slot];
}

/**
 * Acquire a known slot, confirming it still holds the expected identifier.
 *
 * @return The locked entity, or NULL when the slot was recycled or is empty.
 */
NPCEntity* npc_world_acquire_slot(NPCWorld* world, int slot, uint32_t expected_id) {
    if (!world || !world->npcs || expected_id == 0) return NULL;
    if (slot < 0 || slot >= world->capacity) return NULL;

    pthread_rwlock_rdlock(&world->lock);
    pthread_mutex_lock(&world->slot_locks[slot]);

    if (world->npcs[slot].id != expected_id) {
        pthread_mutex_unlock(&world->slot_locks[slot]);
        pthread_rwlock_unlock(&world->lock);
        return NULL;
    }

    return &world->npcs[slot];
}

/**
 * Release an entity acquired from this pool.
 */
void npc_world_release(NPCWorld* world, NPCEntity* npc) {
    if (!world || !world->npcs || !npc) return;

    int slot = (int)(npc - world->npcs);
    if (slot < 0 || slot >= world->capacity) return;

    pthread_mutex_unlock(&world->slot_locks[slot]);
    pthread_rwlock_unlock(&world->lock);
}

/**
 * Find where the nearest living NPC of a type currently stands.
 */
int npc_world_nearest_of_type(NPCWorld* world, uint16_t npc_type_id,
                              float from_x, float from_y,
                              float* out_x, float* out_y) {
    if (!world) return 0;

    int   found   = 0;
    float best_d2 = 0.0f;
    float best_x  = 0.0f;
    float best_y  = 0.0f;

    npc_world_read_begin(world);
    int capacity = npc_world_capacity(world);

    for (int slot = 0; slot < capacity; slot++) {
        NPCEntity* npc = npc_world_slot(world, slot);
        if (!npc || npc->id == 0) continue;

        npc_world_slot_lock(world, slot);
        if (npc->id != 0 && npc->is_alive && npc->npc_type_id == npc_type_id) {
            float dx = npc->pos_x - from_x;
            float dy = npc->pos_y - from_y;
            float d2 = dx * dx + dy * dy;
            if (!found || d2 < best_d2) {
                found   = 1;
                best_d2 = d2;
                best_x  = npc->pos_x;
                best_y  = npc->pos_y;
            }
        }
        npc_world_slot_unlock(world, slot);
    }

    npc_world_read_end(world);

    if (found) {
        if (out_x) *out_x = best_x;
        if (out_y) *out_y = best_y;
    }
    return found;
}
