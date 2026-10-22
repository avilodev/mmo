#ifndef PLAYER_DATA_H
#define PLAYER_DATA_H

#include "types.h"
#include "utils.h"
#include "character_record.h"
#include "quest_state.h"    /**< Define the quest tables PlayerSaveData copies. */

#include <stdint.h>
#include <math.h>

/** @file Manage online-player slots and persistence snapshots.
 * Acquire the registry before a slot, and never call player_acquire() while holding it.
 */

void player_registry_rdlock(void);   /**< Lock shared slot occupancy for reading. */
void player_registry_wrlock(void);   /**< Lock slot occupancy for allocation or removal. */
void player_registry_unlock(void);

// copy occupied indices and return the count written
int player_active_slots(int* out_slots, int max_slots);

// return internal indices valid only under the registry read lock
const int* player_active_list_locked(int* out_count);

// return an advisory occupied-slot count
int player_active_count(void);

// return an advisory slot index or -1 when offline
int player_slot_of(uint32_t character_id);

/** Hold all player state committed together after releasing the slot lock. */
typedef struct {
    character_record scalars;                        // level, position, gold, ...
    ItemInstance     inventory[INVENTORY_SLOTS];
    ItemInstance     equipment[EQUIP_SLOTS];
    /** Copied out of the player under its lock, so the save thread can write
     *  without holding it. Owned by the snapshot; player_snapshot_release()
     *  frees it. One value rather than a flattened array per table, so a table
     *  added to PlayerQuestState needs no change here. */
    PlayerQuestState  quests;
    /** Say whether `quests` is a real copy or an empty stand-in.
     *
     * Clear only when the copy ran out of memory. An empty quest state is
     * indistinguishable from a character who has done nothing, so writing one
     * would erase a completion history that can never be rebuilt -- the save
     * is skipped and reported instead.
     */
    int               quests_copied;
} PlayerSaveData;

/** Copy a player into a snapshot while holding that player's slot lock.
 *
 * The snapshot owns a heap copy of the character's quest state, so every
 * snapshot must be handed to player_snapshot_release() once its save has been
 * committed.
 */
void player_snapshot_for_save(const ActivePlayer* player, PlayerSaveData* out);

/** Free the heap a snapshot owns. Safe on a zeroed or already-released snapshot. */
void player_snapshot_release(PlayerSaveData* snapshot);

// block on PostgreSQL without holding player locks
int player_commit_save(const PlayerSaveData* snapshot);

/** Tell the persistence layer which world it is saving for.
 *
 * Used for the world-session marks the periodic save pass renews, which are
 * what stop the realm deleting a character out from under a live session.
 */
void playerdata_set_world_id(uint32_t world_id);

int playerdata_init(const char* conn_str);

// name the folder that held quest files before quests moved to the database
void playerdata_set_legacy_quest_dir(const char* dir);

// read the highest persisted item-instance id, return 0 when it cannot be read
int playerdata_highest_instance_id(uint64_t* out_highest);

// steps the game logic runs on a player while it is being loaded
typedef struct {
	void (*derive_stats)(ActivePlayer* player);       // compute stats from level
	void (*refresh_abilities)(ActivePlayer* player);  // choose the hotbars
	void (*apply_equipment)(ActivePlayer* player);    // add gear bonuses
} player_load_hooks;

// register the load steps once at startup; this layer never calls game logic
void playerdata_set_load_hooks(const player_load_hooks* hooks);
void playerdata_close(void);
int playerdata_load(uint32_t character_id, ActivePlayer* player);
int playerdata_save(ActivePlayer* player);

// optionally return a cacheable slot index after successful insertion
/** player_add_active() outcomes. */
enum {
    PLAYER_ADD_FAILED  = 0,  /**< No slot, or the database load failed. */
    PLAYER_ADD_READY   = 1,  /**< The slot is loaded and may be used immediately. */
    /** The descriptor was bound to a slot whose load is still in flight.
     *
     * A reconnect that arrives while the first login's database read is still
     * running lands here. It used to be reported as success, and the caller
     * then sent character data, stats and quests read out of a slot that was
     * still all zeroes. Treat it as "connected, not yet populated": the login
     * already in progress publishes the record, and the client asks for its
     * data again over the normal request path. */
    PLAYER_ADD_LOADING = 2,
};

int player_add_active(uint32_t character_id, int client_fd, int* out_slot);

/** Resolve an online character by name, case-insensitively.
 *
 * Answered from an index rather than by scanning every online player and
 * taking each one's mutex, which is what whispers and party invitations used
 * to do -- on the chat dispatch thread and on a network loop thread
 * respectively, at a cost that grew with how busy the world was.
 *
 * @return The character id, or 0 when nobody online carries that name.
 */
uint32_t player_find_by_name(const char* name);

/** Resolve an online character's descriptor by name.
 *
 * One index lookup and one slot lock, rather than a lookup followed by a
 * second resolution through player_acquire().
 *
 * @return The descriptor, or -1 when nobody online carries that name.
 */
int player_fd_by_name(const char* name);

// return a locked matching player that must be released, or NULL
ActivePlayer* player_acquire(uint32_t character_id);

// lock and verify a known slot without taking the registry lock
ActivePlayer* player_acquire_slot(int slot, uint32_t expected_character_id);

// verify a cached slot or fall back to identifier lookup
ActivePlayer* player_acquire_hint(uint32_t character_id, int slot_hint);

void player_release(ActivePlayer* player);

void player_remove_active(uint32_t character_id);
int player_remove_active_if_fd(uint32_t character_id, int client_fd);

/** Keep a dropped connection's character in the world instead of removing it.
 *
 * The slot stays loaded and visible to everyone else, its descriptor becomes -1
 * so nothing is ever sent to a number another connection may now hold, and it
 * stops moving. player_add_active() clears all of this when the character
 * comes back.
 *
 * @param client_fd  The descriptor that closed; nothing happens unless the slot
 *                   still belongs to it, so a reconnect that already rebound the
 *                   slot is left alone.
 * @param now        Seconds on the monotonic clock.
 * @return           1 when the slot is now link-dead, 0 otherwise.
 */
int player_mark_linkdead(uint32_t character_id, int client_fd, double now);

/** List the link-dead characters whose grace period is over.
 *
 * @param grace    Seconds a character may stay link-dead.
 * @param out_ids  Receives up to max_out character ids.
 * @return         How many were written.
 */
int player_linkdead_expired(double now, double grace, uint32_t* out_ids, int max_out);

/** Remove a character that is still link-dead, saving it first if it changed.
 *
 * Does nothing for a character that reconnected after it was listed, so a
 * player who returns in the last moment is not thrown out.
 *
 * @param out_account_id  Receives the owning account when removed; may be NULL.
 * @return                1 when removed, 0 when it had reconnected or was gone.
 */
int player_remove_linkdead(uint32_t character_id, uint32_t* out_account_id);

/** Run a function on every wake of the save thread, which is a few seconds apart.
 *
 * For work that touches the database or Redis and so must not run on the
 * broadcast thread. The function runs with no registry or slot lock held.
 */
void playerdata_set_wake_hook(void (*hook)(void));
void player_send_data_response(int client_fd, uint32_t character_id);

// send wire-numbered slots while acquiring the player lock internally
void player_send_slot_updates(int client_fd, uint32_t character_id,
                              const uint16_t* slot_ids, int count);

/** Mark a player's pending state as one whose loss they would notice.
 *
 * Ordinary changes ride the SAVE_INTERVAL_SECONDS sweep, which is two minutes.
 * A level gained, a quest turned in, coin spent or earned, a rare item picked
 * up -- those are what a player files a ticket about losing, and they get the
 * short pass instead, at PLAYER_SAVE_CRITICAL_SECONDS.
 *
 * Sets is_dirty as well, so a caller never has to set both. Cheap enough to
 * call on the path that already decided the milestone happened; it does no I/O
 * and takes no lock of its own.
 *
 * The caller must hold player->lock, exactly as a bare `is_dirty = 1` requires.
 *
 * Inline because it is two stores and because it is called from files that
 * stub the player registry rather than link it -- the quest and shop suites
 * among them, which would otherwise need a stub for a function that only
 * assigns two fields they already own.
 */
static inline void player_mark_critical(ActivePlayer* player) {
    if (!player) return;
    player->is_dirty          = 1;
    player->is_dirty_critical = 1;
}

/** How often the save thread drains milestone state, in seconds. */
int playerdata_critical_interval(void);

/** How often the save thread sweeps every dirty player, in seconds. */
int playerdata_save_interval(void);

/** Run one drain of pending saves on the calling thread.
 *
 * The same pass the save thread runs, reachable without waiting for its timer.
 * Blocks on the database, so it must not be called with a player lock or the
 * registry lock held.
 *
 * @param critical_only  Nonzero to take only players carrying milestone state,
 *                       zero to take every dirty player.
 * @return The number of characters committed.
 */
int playerdata_flush_saves(int critical_only);

void* periodic_save_thread(void* arg);
int playerdata_start_save_thread(void);
void playerdata_stop_save_thread(void);


#endif // PLAYERDATA_H
