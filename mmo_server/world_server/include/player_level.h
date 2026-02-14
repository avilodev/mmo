#ifndef PLAYER_LEVEL_H
#define PLAYER_LEVEL_H

#include "headers.h"

void player_apply_class_stats(ActivePlayer* player);
void player_apply_equipment_bonuses(ActivePlayer* player);
void player_award_xp(ActivePlayer* player, uint64_t xp_amount);
void player_award_gold(ActivePlayer* player, uint32_t amount);
void player_send_kill_reward(int client_fd, ActivePlayer* player, uint32_t xp, uint32_t gold);
void player_send_stats(int client_fd, ActivePlayer* player);

#endif