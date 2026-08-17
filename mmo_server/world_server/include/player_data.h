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
    PlayerQuestEntry quests[MAX_PLAYER_QUESTS];
    int              quest_count;
} PlayerSaveData;

// copy while holding the source player's slot lock
void player_snapshot_for_save(const ActivePlayer* player, PlayerSaveData* out);

// block on PostgreSQL without holding player locks
int player_commit_save(const PlayerSaveData* snapshot);

int playerdata_init(const char* conn_str);
void playerdata_close(void);
int playerdata_load(uint32_t character_id, ActivePlayer* player);
int playerdata_save(ActivePlayer* player);

// optionally return a cacheable slot index after successful insertion
int player_add_active(uint32_t character_id, int client_fd, int* out_slot);

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
