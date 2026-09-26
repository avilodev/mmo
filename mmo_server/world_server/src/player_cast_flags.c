/** @file Store the per-slot mid-cast flags described in player_cast_flags.h. */

#include "player_cast_flags.h"

_Atomic uint8_t g_player_casting[MAX_PLAYERS];

void player_cast_flag_set(int slot, int casting) {
    if (slot < 0 || slot >= MAX_PLAYERS) return;
    atomic_store_explicit(&g_player_casting[slot], (uint8_t)(casting ? 1 : 0),
                          memory_order_relaxed);
}

int player_cast_flag_get(int slot) {
    if (slot < 0 || slot >= MAX_PLAYERS) return 0;
    return atomic_load_explicit(&g_player_casting[slot], memory_order_relaxed);
}
