#ifndef PLAYER_DATA_H
#define PLAYER_DATA_H

#include "types.h"
#include "utils.h"

#include <stdint.h>
#include <math.h>

int playerdata_init(const char* conn_str);
void playerdata_close(void);
int playerdata_load(uint32_t character_id, ActivePlayer* player);
int playerdata_save(ActivePlayer* player);
int player_add_active(uint32_t character_id, int client_fd);
// Returns a LOCKED ActivePlayer* that is guaranteed to still be loaded and
// match the given character_id. Caller MUST call player_release() when done.
// Returns NULL if the player is not found or was removed between lookup and lock.
ActivePlayer* player_acquire(uint32_t character_id);

// Unlocks a player previously returned by player_acquire().
void player_release(ActivePlayer* player);

void player_remove_active(uint32_t character_id);
void player_send_data_response(int client_fd, uint32_t character_id);

void* periodic_save_thread(void* arg);
int playerdata_start_save_thread(void);
void playerdata_stop_save_thread(void);


#endif // PLAYERDATA_H