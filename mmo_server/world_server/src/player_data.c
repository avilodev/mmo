/**
 * @file
 * Manage active-player slots, persistence, synchronized access, and player-data packets.
 */

#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "types.h"
#include "log.h"

#include "ability_def.h"
#include "items_database.h"
#include "player_data.h"
#include "players_database.h"
#include "class_stats.h"
#include "player_effects.h"
#include "ability_handler.h"
#include "move_validator.h"
#include "player_level.h"
#include "quest_system.h"
#include "world_regions.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <sys/time.h>
#include <stddef.h>   // offsetof
#include <arpa/inet.h>
#include <sys/socket.h>

#define SAVE_INTERVAL_SECONDS 120

/** Bound one persistence batch.
 *
 * PlayerSaveData is roughly 6 KB, so a queue sized MAX_PLAYERS is 5.7 MB — past a
 * musl thread stack by 40x and most of a glibc one. Batching keeps the buffer small
 * enough to allocate once on the heap and also caps how long the registry read lock
 * is held in a single pass.
 */
#define SAVE_BATCH_SIZE 64

ActivePlayer active_players[MAX_PLAYERS];

// protects slot occupancy, the identifier index, and active-slot list
static pthread_rwlock_t g_registry_lock = PTHREAD_RWLOCK_INITIALIZER;

/** Acquire the shared player-registry lock. */
void player_registry_rdlock(void) { pthread_rwlock_rdlock(&g_registry_lock); }
/** Acquire the exclusive player-registry lock. */
void player_registry_wrlock(void) { pthread_rwlock_wrlock(&g_registry_lock); }
/** Release the held player-registry lock. */
void player_registry_unlock(void) { pthread_rwlock_unlock(&g_registry_lock); }

#define PLAYER_INDEX_CAP   2048           // power of two, > 2 * MAX_PLAYERS
#define PLAYER_INDEX_MASK  (PLAYER_INDEX_CAP - 1)

#define IDX_EMPTY      (-1)
#define IDX_TOMBSTONE  (-2)

typedef struct {
    uint32_t key;    // character_id
    int32_t  slot;   // >= 0 live, IDX_EMPTY, or IDX_TOMBSTONE
} IndexEntry;

static IndexEntry g_index[PLAYER_INDEX_CAP];
static int        g_index_tombstones = 0;

// includes loaded and reserved slots
static int g_active_slots[MAX_PLAYERS];
static int g_active_count = 0;
static int g_slot_position[MAX_PLAYERS];   // slot -> index into g_active_slots, or -1

static inline uint32_t index_hash(uint32_t key) {
    return (key * 2654435769u) >> 21;   // top 11 bits -> 2048 buckets
}

static void index_reset(void) {
    for (int i = 0; i < PLAYER_INDEX_CAP; i++) {
        g_index[i].key  = 0;
        g_index[i].slot = IDX_EMPTY;
    }
    g_index_tombstones = 0;
}

/**
 * Find a character in the open-addressed slot index.
 *
 * The caller must hold at least the registry read lock.
 *
 * @return The slot index, or -1 when absent.
 */
static int index_find(uint32_t character_id) {
    uint32_t pos = index_hash(character_id) & PLAYER_INDEX_MASK;
    for (int probe = 0; probe < PLAYER_INDEX_CAP; probe++) {
        const IndexEntry* e = &g_index[pos];
        if (e->slot == IDX_EMPTY) return -1;                  // chain ends: miss
        if (e->slot >= 0 && e->key == character_id) return e->slot;
        pos = (pos + 1) & PLAYER_INDEX_MASK;                  // tombstone or collision
    }
    return -1;
}

/**
 * Insert or rebind a character in the slot index.
 *
 * The caller must hold the registry write lock.
 */
static void index_insert(uint32_t character_id, int slot) {
    uint32_t pos = index_hash(character_id) & PLAYER_INDEX_MASK;
    int reuse = -1;
    for (int probe = 0; probe < PLAYER_INDEX_CAP; probe++) {
        IndexEntry* e = &g_index[pos];
        if (e->slot == IDX_TOMBSTONE) {
            // probe past tombstones to avoid shadowing a live duplicate
            if (reuse < 0) reuse = (int)pos;
        } else if (e->slot == IDX_EMPTY) {
            if (reuse >= 0) { g_index_tombstones--; pos = (uint32_t)reuse; }
            g_index[pos].key  = character_id;
            g_index[pos].slot = slot;
            return;
        } else if (e->key == character_id) {
            e->slot = slot;      // rebind an existing character to a new slot
            return;
        }
        pos = (pos + 1) & PLAYER_INDEX_MASK;
    }
    // Unreachable while capacity > MAX_PLAYERS, but never silently corrupt.
    LOG_ERROR("[INDEX] table full inserting character %u", character_id);
}

/**
 * Rebuild the identifier index from occupied slots.
 *
 * The caller must hold the registry write lock.
 */
static void index_rebuild(void) {
    index_reset();
    for (int i = 0; i < g_active_count; i++) {
        int slot = g_active_slots[i];
        index_insert(active_players[slot].character_id, slot);
    }
}

/**
 * Remove a character from the slot index and collect excess tombstones.
 *
 * The caller must hold the registry write lock.
 */
static void index_remove(uint32_t character_id) {
    uint32_t pos = index_hash(character_id) & PLAYER_INDEX_MASK;
    for (int probe = 0; probe < PLAYER_INDEX_CAP; probe++) {
        IndexEntry* e = &g_index[pos];
        if (e->slot == IDX_EMPTY) return;
        if (e->slot >= 0 && e->key == character_id) {
            e->slot = IDX_TOMBSTONE;
            e->key  = 0;
            g_index_tombstones++;
            break;
        }
        pos = (pos + 1) & PLAYER_INDEX_MASK;
    }

    // rebuild after sustained login and logout churn
    if (g_index_tombstones > PLAYER_INDEX_CAP / 4) index_rebuild();
}

static void active_list_add(int slot) {
    if (g_slot_position[slot] >= 0) return;          // already listed
    g_slot_position[slot] = g_active_count;
    g_active_slots[g_active_count++] = slot;
}

static void active_list_remove(int slot) {
    int pos = g_slot_position[slot];
    if (pos < 0) return;
    int last = g_active_slots[--g_active_count];     // swap the tail into the hole
    g_active_slots[pos] = last;
    g_slot_position[last] = pos;
    g_slot_position[slot] = -1;
}

/**
 * Copy occupied slot indices under the registry read lock.
 *
 * The snapshot may be stale immediately after return.
 *
 * @param out_slots  Destination array.
 * @param max_slots  Destination capacity.
 * @return           The number of indices copied.
 */
int player_active_slots(int* out_slots, int max_slots) {
    if (!out_slots || max_slots <= 0) return 0;
    player_registry_rdlock();
    int n = g_active_count < max_slots ? g_active_count : max_slots;
    memcpy(out_slots, g_active_slots, (size_t)n * sizeof(int));
    player_registry_unlock();
    return n;
}

/**
 * Resolve a loaded character to its current slot index.
 *
 * @return The slot index, or -1 when offline.
 */
int player_slot_of(uint32_t character_id) {
    player_registry_rdlock();
    int slot = index_find(character_id);
    if (slot >= 0 && !active_players[slot].is_loaded) slot = -1;
    player_registry_unlock();
    return slot;
}

/**
 * Borrow the occupied-slot list while holding the registry lock.
 *
 * The returned pointer is valid only until the caller releases that lock.
 *
 * @param out_count  Receives the number of entries; may be NULL.
 * @return           The internal occupied-slot array.
 */
const int* player_active_list_locked(int* out_count) {
    if (out_count) *out_count = g_active_count;
    return g_active_slots;
}

/**
 * Report the current occupied-slot count.
 *
 * @return An advisory count sampled under the registry read lock.
 */
int player_active_count(void) {
    player_registry_rdlock();
    int n = g_active_count;
    player_registry_unlock();
    return n;
}

static pthread_t g_save_thread;
static pthread_mutex_t g_save_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_save_cond = PTHREAD_COND_INITIALIZER;
static volatile int g_save_thread_running = 0;

/**
 * Clear a player slot without overwriting its trailing mutex.
 *
 * The caller must hold the registry write lock followed by the slot lock.
 */
static void player_slot_clear(int slot) {
    uint32_t character_id = active_players[slot].character_id;

    // remove from the rebuild source before tombstoning the index
    active_list_remove(slot);
    if (character_id != 0) index_remove(character_id);

    memset(&active_players[slot], 0, offsetof(ActivePlayer, lock));
}

/**
 * Replace the unused initial registry lock with a writer-preferring lock when supported.
 *
 * Call this before any worker can acquire the registry lock.
 */
static void registry_lock_prefer_writers(void) {
#if defined(__GLIBC__)
    pthread_rwlockattr_t attr;
    if (pthread_rwlockattr_init(&attr) != 0) return;

    if (pthread_rwlockattr_setkind_np(
            &attr, PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP) == 0) {
        pthread_rwlock_destroy(&g_registry_lock);
        if (pthread_rwlock_init(&g_registry_lock, &attr) != 0) {
            // Fall back to a default lock rather than leaving it destroyed.
            pthread_rwlock_init(&g_registry_lock, NULL);
            LOG_ERROR("[REGISTRY] writer-preferring lock unavailable; "
                      "logins may queue behind readers under load");
        }
    }
    pthread_rwlockattr_destroy(&attr);
#else
    LOG_WARN("[REGISTRY] platform lacks writer-preferring rwlocks; "
             "logins may queue behind readers under load");
#endif
}

/**
 * Copy persistable state from a locked player into a save snapshot.
 *
 * The caller must hold the player's slot lock.
 */
void player_snapshot_for_save(const ActivePlayer* player, PlayerSaveData* out) {
    if (!player || !out) return;

    memset(out, 0, sizeof(*out));

    CharacterInfo* d = &out->scalars;
    d->character_id = player->character_id;
    d->level        = player->level;
    d->pos_x        = player->pos_x;
    d->pos_y        = player->pos_y;
    d->health       = player->health;
    d->max_health   = player->max_health;
    d->resource     = player->resource;
    d->max_resource = player->max_resource;
    d->experience   = player->experience;
    memcpy(d->currency, player->currency, sizeof(d->currency));

    memcpy(out->inventory, player->inventory, sizeof(out->inventory));
    memcpy(out->equipment, player->equipment, sizeof(out->equipment));

    out->quest_count = player->quest_count;
    if (out->quest_count > MAX_PLAYER_QUESTS) out->quest_count = MAX_PLAYER_QUESTS;
    memcpy(out->quests, player->quests,
           (size_t)out->quest_count * sizeof(out->quests[0]));
}

/**
 * Persist scalar, item, and quest portions of a player snapshot.
 *
 * This function performs blocking database and file I/O and must be called without player locks.
 *
 * @return 1 when every portion succeeds, or 0 after any partial or total failure.
 */
int player_commit_save(const PlayerSaveData* snapshot) {
    if (!snapshot) return 0;

    uint32_t character_id = snapshot->scalars.character_id;
    int ok = 1;

    if (!character_update_full_data(&snapshot->scalars)) {
        LOG_ERROR("Failed to save character %u", character_id);
        ok = 0;
    }

    // scalar and item writes can fail independently
    if (!character_items_save(character_id,
                              snapshot->inventory, INVENTORY_SLOTS,
                              snapshot->equipment, EQUIP_SLOTS)) {
        LOG_ERROR("Failed to save items for character %u", character_id);
        ok = 0;
    }

    if (!quest_player_save(character_id, snapshot->quests, snapshot->quest_count)) {
        LOG_ERROR("Failed to save quests for character %u", character_id);
        ok = 0;
    }

    return ok;
}

/**
 * Initialize the character database, registry structures, and slot mutexes.
 *
 * @return 1 on success, or 0 when database initialization fails.
 */
int playerdata_init(const char* conn_str) {
    LOG_DEBUG("Initializing player data system...");

    registry_lock_prefer_writers();
    LOG_DEBUG("PostgreSQL connection: %s", conn_str);

    // Initialize the character database
    if (!character_database_init(conn_str)) {
        LOG_ERROR("Failed to initialize character database");
        return 0;
    }

    // Initialize active players array
    memset(active_players, 0, sizeof(active_players));
    for (int i = 0; i < MAX_PLAYERS; i++) {
        pthread_mutex_init(&active_players[i].lock, NULL);
        g_slot_position[i] = -1;
    }
    g_active_count = 0;
    index_reset();

    LOG_INFO("Player data system initialized successfully");
    return 1;
}

/**
 * Save loaded players, destroy slot mutexes, and close the character database.
 *
 * This function performs blocking persistence after releasing registry and slot locks.
 */
void playerdata_close(void) {
    LOG_DEBUG("Closing player data system...");

    /* Snapshot all loaded players, clear slots, then save lock-free.
     * On the heap: MAX_PLAYERS PlayerSaveData is 5.7 MB, well past a thread stack. */
    PlayerSaveData* save_buf = calloc(MAX_PLAYERS, sizeof(PlayerSaveData));
    int save_count = 0;

    if (!save_buf) {
        LOG_ERROR("Shutdown save buffer allocation failed; "
                  "unsaved player state will be lost");
    }

    player_registry_wrlock();
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (save_buf && active_players[i].is_loaded) {
            pthread_mutex_lock(&active_players[i].lock);
            player_snapshot_for_save(&active_players[i], &save_buf[save_count++]);
            pthread_mutex_unlock(&active_players[i].lock);
        }
        pthread_mutex_destroy(&active_players[i].lock);
    }
    g_active_count = 0;
    index_reset();
    player_registry_unlock();

    for (int i = 0; i < save_count; i++) {
        if (!player_commit_save(&save_buf[i]))
            LOG_ERROR("Shutdown save failed for character %u",
                      save_buf[i].scalars.character_id);
    }
    free(save_buf);

    character_database_close();

    LOG_DEBUG("Player data system closed");
}

/**
 * Load database, item, quest, ability, and derived-stat state into a player.
 *
 * @param player  Destination staging or active-player object; may not be NULL.
 * @return        1 when character scalar data loads, or 0 otherwise.
 */
int playerdata_load(uint32_t character_id, ActivePlayer* player) {
    if (!player) {
        LOG_ERROR("playerdata_load: NULL player pointer");
        return 0;
    }

    // Get full character data from database
    CharacterInfo char_info;
    memset(&char_info, 0, sizeof(char_info));

    if (!character_get_full_data(character_id, &char_info)) {
        LOG_ERROR("Failed to load character %u from database", character_id);
        return 0;
    }

    LOG_DEBUG("[LOAD DEBUG] char_id=%u from DB: pos_x=%f, pos_y=%f", character_id, char_info.pos_x, char_info.pos_y);

    // Copy data into ActivePlayer structure
    player->character_id = char_info.character_id;
    strncpy(player->username, char_info.name, sizeof(player->username) - 1);
    player->username[sizeof(player->username) - 1] = '\0';

    player->pos_x = char_info.pos_x;
    player->pos_y = char_info.pos_y;

    // assign the default spawn to unplaced characters
    if (player->pos_x == 0.0f && player->pos_y == 0.0f) {
        // Ennara Courtyard centre, from the shared city table.
        const WorldCity* start = world_city_find(CITY_ENNARA);
        world_city_center_px(start, &player->pos_x, &player->pos_y);
        player->is_dirty = 1;  // Mark for save so this persists
        LOG_DEBUG("[SPAWN] New character %u spawned at default location (%.1f, %.1f)", character_id, player->pos_x, player->pos_y);
    }

    player->vel_x = 0.0f;
    player->vel_y = 0.0f;

    player->level = char_info.level;
    player->health = char_info.health;
    player->max_health = char_info.max_health;
    player->resource = char_info.resource;
    player->max_resource = char_info.max_resource;
    player->experience = char_info.experience;
    memcpy(player->currency, char_info.currency, sizeof(player->currency));

    player->race_id = char_info.race_id;

    /* Characters enter the world in Animal Form: it is the form that carries the
     * race's kit, its passive, and its resource pool. */
    player->form = FORM_ANIMAL;
    player->form_swap_ready_at = 0.0;
    memset(player->ability_ready_at, 0, sizeof(player->ability_ready_at));

    player_recompute_stats(player);

    /* A first login has the schema's default health rather than a real value. */
    if (player->health <= 0 || player->health > player->max_health) {
        player->health = player->max_health;
    }

    /* Rage builds through combat rather than being granted, so a tank logs in with an
     * empty bar; mana and stamina start full. */
    player->resource = (player->resource_type == RESOURCE_RAGE) ? 0 : player->max_resource;

    ability_refresh_hotbars(player);
    LOG_DEBUG("[ABILITIES] Player %u (race %u, level %d): %d human / %d animal abilities",
              player->character_id, player->race_id, player->level,
              player->ability_count[FORM_HUMAN], player->ability_count[FORM_ANIMAL]);


    // clear both item arrays after any item-load failure
    if (!character_items_load(character_id,
                              player->inventory, INVENTORY_SLOTS,
                              player->equipment, EQUIP_SLOTS)) {
        LOG_ERROR("Failed to load items for character %u", character_id);
        memset(player->inventory, 0, sizeof(player->inventory));
        memset(player->equipment, 0, sizeof(player->equipment));
    }

    // Load quest state from file
    player->quest_count = quest_player_load(character_id, (PlayerQuestEntry*)player->quests,
                                             MAX_PLAYER_QUESTS);

    // Now that equipment is loaded, recalculate stats with gear bonuses
    player_apply_equipment_bonuses(player);

    player->is_loaded = 1;
    player->is_dirty = player->is_dirty ? 1 : 0;  // Keep dirty flag if we set spawn
    player->last_save = time(NULL);
    player->last_activity = time(NULL);
    struct timespec load_tv;
    clock_gettime(CLOCK_MONOTONIC, &load_tv);
    move_budget_reset(&player->move_budget, &load_tv);

    LOG_INFO("Loaded character %u: %s (level %d) at pos=(%.2f, %.2f)", character_id, player->username, player->level, player->pos_x, player->pos_y);

    return 1;
}

/**
 * Snapshot and save one loaded player while retaining its caller-held lock.
 *
 * This function blocks on persistence; unlocked paths should use player_snapshot_for_save() and player_commit_save().
 *
 * @return 1 on success, or 0 for invalid state or persistence failure.
 */
int playerdata_save(ActivePlayer* player) {
    if (!player || !player->is_loaded) {
        LOG_ERROR("playerdata_save: Invalid player or not loaded");
        return 0;
    }

    PlayerSaveData snapshot;
    player_snapshot_for_save(player, &snapshot);
    snprintf(snapshot.scalars.name, sizeof(snapshot.scalars.name), "%s",
             player->username);

    if (!player_commit_save(&snapshot)) return 0;

    player->is_dirty = 0;
    player->last_save = time(NULL);

    LOG_DEBUG("Saved character %u to database", player->character_id);
    return 1;
}

/**
 * Reserve, load, and publish an active-player slot or rebind an existing slot.
 *
 * Database and quest-file reads occur without registry or slot locks.
 *
 * @param out_slot  Receives the occupied slot on success; may be NULL.
 * @return          1 on success, or 0 when no slot is available or loading fails.
 */
int player_add_active(uint32_t character_id, int client_fd, int* out_slot) {
    if (out_slot) *out_slot = -1;

    // reserve a slot without performing I/O
    player_registry_wrlock();

    // rebind reconnects to loaded or reserved slots
    int existing = index_find(character_id);
    if (existing >= 0) {
        pthread_mutex_lock(&active_players[existing].lock);
        active_players[existing].client_fd = client_fd;
        active_players[existing].is_ready = 0;
        pthread_mutex_unlock(&active_players[existing].lock);
        player_registry_unlock();
        if (out_slot) *out_slot = existing;
        LOG_DEBUG("[PLAYER_ADD] char=%u rebound to fd=%d in slot=%d", character_id, client_fd, existing);
        return 1;
    }

    // Find an empty slot — must skip slots reserved by an in-flight login.
    int slot = -1;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (!active_players[i].is_loaded && !active_players[i].is_reserved) {
            slot = i;
            break;
        }
    }

    if (slot == -1) {
        player_registry_unlock();
        LOG_ERROR("[PLAYER_ADD] char=%u fd=%d: no available slots!", character_id, client_fd);
        return 0;
    }

    pthread_mutex_lock(&active_players[slot].lock);
    player_slot_clear(slot);
    active_players[slot].is_reserved  = 1;   // claimed, not yet visible to gameplay
    active_players[slot].character_id = character_id;
    active_players[slot].client_fd    = client_fd;
    // Publish to the index while still reserved, so a racing reconnect finds
    // this login and rebinds to it instead of starting a second DB load.
    index_insert(character_id, slot);
    active_list_add(slot);
    pthread_mutex_unlock(&active_players[slot].lock);

    player_registry_unlock();

    // load into staging without holding registry or slot locks
    LOG_DEBUG("[PLAYER_ADD] char=%u fd=%d: slot=%d loading from DB (unlocked)", character_id, client_fd, slot);

    ActivePlayer staging;
    memset(&staging, 0, sizeof(staging));
    staging.character_id = character_id;
    staging.client_fd    = client_fd;

    int loaded = playerdata_load(character_id, &staging);

    // publish the completed staging record under both locks
    player_registry_wrlock();
    pthread_mutex_lock(&active_players[slot].lock);

    if (!loaded) {
        player_slot_clear(slot);   // drops the reservation, frees the slot
        pthread_mutex_unlock(&active_players[slot].lock);
        player_registry_unlock();
        LOG_ERROR("[PLAYER_ADD] char=%u fd=%d: DB load failed!", character_id, client_fd);
        return 0;
    }

    // preserve any descriptor rebound during loading
    int current_fd = active_players[slot].client_fd;

    // Copy every field except the trailing mutex, which must stay the one
    // this thread currently holds.
    memcpy(&active_players[slot], &staging, offsetof(ActivePlayer, lock));

    active_players[slot].client_fd   = current_fd;
    active_players[slot].is_reserved = 0;
    active_players[slot].is_loaded   = 1;   // now visible to gameplay scans

    pthread_mutex_unlock(&active_players[slot].lock);
    player_registry_unlock();

    if (out_slot) *out_slot = slot;
    LOG_INFO("[PLAYER_ADD] char=%u fd=%d: slot=%d loaded successfully", character_id, client_fd, slot);
    return 1;
}

/**
 * Borrow an active-player pointer without locking its slot.
 *
 * The returned pointer may become stale immediately; use player_acquire() when accessing contents.
 *
 * @return The current pool address, or NULL when offline.
 */
ActivePlayer* player_find_active(uint32_t character_id) {
    player_registry_rdlock();
    int slot = index_find(character_id);
    int ok = (slot >= 0 && active_players[slot].is_loaded);
    player_registry_unlock();
    return ok ? &active_players[slot] : NULL;
}

/**
 * Lock a known slot and verify that it still holds the expected character.
 *
 * The caller must release a successful result with player_release().
 *
 * @return The locked player, or NULL for an invalid, unloaded, or recycled slot.
 */
ActivePlayer* player_acquire_slot(int slot, uint32_t expected_character_id) {
    if (slot < 0 || slot >= MAX_PLAYERS) return NULL;

    // fixed slot addresses permit direct mutex acquisition
    ActivePlayer* p = &active_players[slot];
    pthread_mutex_lock(&p->lock);

    // reject slots recycled to another character
    if (!p->is_loaded || p->character_id != expected_character_id) {
        pthread_mutex_unlock(&p->lock);
        return NULL;
    }
    return p;
}

/**
 * Acquire a character through a cached slot hint with indexed fallback.
 *
 * The caller must release a successful result with player_release().
 *
 * @return The locked player, or NULL when offline.
 */
ActivePlayer* player_acquire_hint(uint32_t character_id, int slot_hint) {
    if (slot_hint >= 0) {
        ActivePlayer* p = player_acquire_slot(slot_hint, character_id);
        if (p) return p;
        // Hint was stale or the connection never authenticated. Fall through to
        // the index rather than reporting the player offline on a bad guess.
    }
    return player_acquire(character_id);
}

/**
 * Resolve, lock, and revalidate an active character.
 *
 * The caller must release a successful result with player_release().
 *
 * @return The locked player, or NULL when offline or concurrently removed.
 */
ActivePlayer* player_acquire(uint32_t character_id) {
    player_registry_rdlock();
    int slot = index_find(character_id);
    player_registry_unlock();

    if (slot < 0) return NULL;

    // release the registry before waiting for the slot mutex
    return player_acquire_slot(slot, character_id);
}

/**
 * Unlock a player returned by an acquire function.
 */
void player_release(ActivePlayer* player) {
    if (player) {
        pthread_mutex_unlock(&player->lock);
    }
}

/**
 * Remove an active character and persist dirty state after releasing locks.
 */
void player_remove_active(uint32_t character_id) {
    LOG_DEBUG("[PLAYER_REMOVE] char=%u: acquiring registry write lock", character_id);
    player_registry_wrlock();

    PlayerSaveData save_data;
    int do_save = 0;

    {
        int i = index_find(character_id);
        if (i >= 0 && active_players[i].is_loaded) {

            int slot_fd = active_players[i].client_fd;
            LOG_DEBUG("[PLAYER_REMOVE] char=%u: found in slot=%d (fd=%d), clearing", character_id, i, slot_fd);
            pthread_mutex_lock(&active_players[i].lock);

            if (active_players[i].is_dirty) {
                // Copy out while holding the lock, write after releasing it.
                player_snapshot_for_save(&active_players[i], &save_data);
                do_save = 1;
            }

            player_slot_clear(i);

            pthread_mutex_unlock(&active_players[i].lock);
            LOG_DEBUG("[PLAYER_REMOVE] char=%u: slot=%d cleared (was fd=%d)", character_id, i, slot_fd);
        }
    }

    player_registry_unlock();

    // DB write is done after all locks are released
    if (do_save) {
        LOG_DEBUG("[PLAYER_REMOVE] char=%u: writing to DB (lock-free)", character_id);
        if (!player_commit_save(&save_data))
            LOG_ERROR("[PLAYER_REMOVE] char=%u: save failed", character_id);
    }

    LOG_DEBUG("[PLAYER_REMOVE] char=%u: done", character_id);
}

/**
 * Remove a character only when its current connection matches a descriptor.
 *
 * @return 1 when removed, or 0 after a mismatch or absence.
 */
int player_remove_active_if_fd(uint32_t character_id, int client_fd) {
    int removed = 0;
    player_registry_wrlock();
    int i = index_find(character_id);
    if (i >= 0 &&
        active_players[i].is_loaded &&
        active_players[i].client_fd == client_fd) {
        pthread_mutex_lock(&active_players[i].lock);
        // Recheck after acquiring the slot lock; a reconnect may have
        // rebound it while this cleanup thread was waiting.
        if (active_players[i].is_loaded &&
            active_players[i].character_id == character_id &&
            active_players[i].client_fd == client_fd) {
            player_slot_clear(i);
            removed = 1;
        }
        pthread_mutex_unlock(&active_players[i].lock);
    }
    player_registry_unlock();
    return removed;
}

/**
 * Encode one item instance for an inventory packet.
 */
static void pack_slot(InventorySlotData* out, const ItemInstance* in) {
    out->instance_id = mmo_htonll(in->instance_id);
    out->item_id     = htonl(in->item_id);
    out->quantity    = htons(in->quantity);
    out->is_bound    = in->is_bound;
    out->_reserved   = 0;
}

/**
 * Send authoritative contents for selected inventory or equipment slots.
 *
 * Acquires the player's slot lock internally.
 *
 * @param slot_ids  Wire-numbered slot identifiers.
 * @param count     Number of identifiers, capped at MAX_SLOT_UPDATES.
 */
void player_send_slot_updates(int client_fd, uint32_t character_id,
                              const uint16_t* slot_ids, int count) {
    if (client_fd < 0 || !slot_ids || count <= 0) return;
    if (count > MAX_SLOT_UPDATES) count = MAX_SLOT_UPDATES;

    ActivePlayer* player = player_acquire(character_id);
    if (!player) return;

    InventoryUpdatePacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.header.type      = PACKET_INVENTORY_UPDATE;
    pkt.header.player_id = htonl(character_id);

    int n = 0;
    for (int i = 0; i < count; i++) {
        uint16_t slot = slot_ids[i];
        const ItemInstance* src = NULL;

        if (slot < INVENTORY_SLOTS) {
            src = &player->inventory[slot];
        } else if (slot >= EQUIP_SLOT_BASE && slot < EQUIP_SLOT_BASE + EQUIP_SLOTS) {
            src = &player->equipment[slot - EQUIP_SLOT_BASE];
        } else {
            LOG_WARN_RL(5, 60, "[INVENTORY] refusing to broadcast slot %u", slot);
            continue;
        }

        pkt.slots[n].slot = htons(slot);
        pack_slot(&pkt.slots[n].data, src);
        n++;
    }
    player_release(player);

    if (n == 0) return;
    pkt.count = (uint8_t)n;

    // transmit only populated update entries
    size_t send_size = offsetof(InventoryUpdatePacket, slots) +
                       (size_t)n * sizeof(SlotUpdateEntry);
    pkt.header.payload_size = htons((uint16_t)(send_size - sizeof(PacketHeader)));
    server_send(client_fd, &pkt, send_size);
}

/**
 * Send a character's scalar, inventory, and equipment state.
 */
void player_send_data_response(int client_fd, uint32_t character_id) {
    ActivePlayer* player = player_acquire(character_id);
    if (!player) {
        LOG_ERROR("Cannot send data for inactive player %u", character_id);
        return;
    }

    CharacterInfo* response = malloc(sizeof(CharacterInfo));
    if (!response) {
        LOG_ERROR("Failed to allocate response packet");
        player_release(player);
        return;
    }

    memset(response, 0, sizeof(CharacterInfo));

    response->header.type = PACKET_PLAYER_DATA_RESPONSE;
    response->header.player_id = htonl(character_id);
    response->header.payload_size = htons(sizeof(CharacterInfo) - sizeof(PacketHeader));

    // Basic stats
    response->level = htonl(player->level);
    response->health = htonl(player->health);
    response->max_health = htonl(player->max_health);
    response->resource = (int32_t)htonl((uint32_t)player->resource);
    response->max_resource = (int32_t)htonl((uint32_t)player->max_resource);
    response->experience = mmo_htonll(player->experience);
    for (int c = 0; c < CURRENCY_COUNT; c++)
        response->currency[c] = htonl(player->currency[c]);

    response->pos_x = player->pos_x;
    response->pos_y = player->pos_y;

    snprintf(response->name, sizeof(response->name), "%s", player->username);
    response->race_id       = htonl(player->race_id);
    response->resource_type = player->resource_type;
    response->form          = player->form;

    // encode complete inventory and equipment slots
    for (int i = 0; i < INVENTORY_SLOTS; i++)
        pack_slot(&response->inventory[i], &player->inventory[i]);
    for (int i = 0; i < EQUIP_SLOTS; i++)
        pack_slot(&response->equipment[i], &player->equipment[i]);

    player_release(player);

    // Send packet
    server_send(client_fd, response, sizeof(CharacterInfo));
    free(response);

    LOG_DEBUG("Sent player data for character %u", character_id);
}

/**
 * Periodically snapshot and persist dirty active players.
 *
 * Waits on the save condition and performs database I/O without registry or slot locks.
 *
 * @return Always NULL when shutdown is requested.
 */
void* periodic_save_thread(void* arg) {
    (void)arg;
    g_save_thread_running = 1;

    /* One heap buffer for the life of the thread. See SAVE_BATCH_SIZE. */
    PlayerSaveData* save_queue = calloc(SAVE_BATCH_SIZE, sizeof(PlayerSaveData));
    if (!save_queue) {
        LOG_ERROR("Periodic save thread cannot allocate its %zu-byte batch buffer; "
                  "player state will only be persisted on logout and shutdown",
                  SAVE_BATCH_SIZE * sizeof(PlayerSaveData));
        g_save_thread_running = 0;
        return NULL;
    }

    /* Cap the passes per interval so a database outage cannot spin here: a failed
     * commit marks the player dirty again, and without a bound the drain below would
     * pick them straight back up. */
    const int max_passes = (MAX_PLAYERS + SAVE_BATCH_SIZE - 1) / SAVE_BATCH_SIZE;

    while (1) {

        struct timespec deadline;
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_sec += SAVE_INTERVAL_SECONDS;

        pthread_mutex_lock(&g_save_mutex);
        while (g_save_thread_running) {
            int ret = pthread_cond_timedwait(&g_save_cond, &g_save_mutex, &deadline);
            if (ret == ETIMEDOUT || !g_save_thread_running) break;
        }
        pthread_mutex_unlock(&g_save_mutex);

        // If we were told to stop, exit immediately
        if (!g_save_thread_running) break;

        /* Drain in batches. is_dirty is cleared under the slot lock as each player is
         * snapshotted, so restarting the scan from the top never saves anyone twice. */
        for (int pass = 0; pass < max_passes; pass++) {
            int save_count = 0;

            // retain shared occupancy access while locking individual slots
            player_registry_rdlock();
            int online_count = 0;
            const int* online = player_active_list_locked(&online_count);
            for (int s_i = 0; s_i < online_count && save_count < SAVE_BATCH_SIZE; s_i++) {
                int i = online[s_i];
                if (!active_players[i].is_loaded) continue;
                pthread_mutex_lock(&active_players[i].lock);
                if (active_players[i].is_dirty) {
                    player_snapshot_for_save(&active_players[i], &save_queue[save_count++]);
                    active_players[i].is_dirty = 0;  // cleared while we hold the lock
                }
                pthread_mutex_unlock(&active_players[i].lock);
            }
            player_registry_unlock();

            if (save_count == 0) break;

            // Now write to DB without holding any locks
            for (int i = 0; i < save_count; i++) {
                uint32_t character_id = save_queue[i].scalars.character_id;
                LOG_DEBUG("Periodic save: character %u", character_id);

                if (!player_commit_save(&save_queue[i])) {
                    // restore dirty state after a failed commit
                    ActivePlayer* player = player_acquire(character_id);
                    if (player) {
                        player->is_dirty = 1;
                        player_release(player);
                    }
                    LOG_ERROR("Periodic save failed for character %u; queued for retry", character_id);
                }
            }

            /* A short batch means the scan found nothing more waiting. Stopping here
             * also keeps a commit failure from being retried within this interval. */
            if (save_count < SAVE_BATCH_SIZE) break;
        }
    }

    free(save_queue);

    LOG_DEBUG("Periodic save thread exiting");
    return NULL;
}

/**
 * Start the periodic-save worker unless it is already running.
 *
 * @return 1 when running, or 0 when thread creation fails.
 */
int playerdata_start_save_thread(void) {
    if (g_save_thread_running) return 1;

    if (pthread_create(&g_save_thread, NULL, periodic_save_thread, NULL) != 0) {
        LOG_ERROR("Failed to create periodic save thread");
        return 0;
    }
    return 1;
}

/**
 * Signal and join the periodic-save worker.
 */
void playerdata_stop_save_thread(void) {
    if (!g_save_thread_running) return;

    pthread_mutex_lock(&g_save_mutex);
    g_save_thread_running = 0;
    pthread_cond_signal(&g_save_cond);
    pthread_mutex_unlock(&g_save_mutex);

    pthread_join(g_save_thread, NULL);
}
