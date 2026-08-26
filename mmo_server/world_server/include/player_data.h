#ifndef PLAYER_DATA_H
#define PLAYER_DATA_H

#include "types.h"
#include "utils.h"
#include "quest_system.h"   /**< Define PlayerQuestEntry used by PlayerSaveData. */

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
    CharacterInfo    scalars;                        // level, position, gold, ...
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

ActivePlayer* player_find_active(uint32_t character_id);

// return a locked matching player that must be released, or NULL
ActivePlayer* player_acquire(uint32_t character_id);

// lock and verify a known slot without taking the registry lock
ActivePlayer* player_acquire_slot(int slot, uint32_t expected_character_id);

// verify a cached slot or fall back to identifier lookup
ActivePlayer* player_acquire_hint(uint32_t character_id, int slot_hint);

void player_release(ActivePlayer* player);

void player_remove_active(uint32_t character_id);
int player_remove_active_if_fd(uint32_t character_id, int client_fd);
void player_send_data_response(int client_fd, uint32_t character_id);

// send wire-numbered slots while acquiring the player lock internally
void player_send_slot_updates(int client_fd, uint32_t character_id,
                              const uint16_t* slot_ids, int count);

void* periodic_save_thread(void* arg);
int playerdata_start_save_thread(void);
void playerdata_stop_save_thread(void);


#endif // PLAYERDATA_H
