#ifndef PLAYER_LEVEL_H
#define PLAYER_LEVEL_H

#include "headers.h"

void player_apply_class_stats(ActivePlayer* player);
void player_apply_equipment_bonuses(ActivePlayer* player);
// Recalculate base stats then re-apply any still-active EFFECT_BUFF effects.
// Call this when a buff expires to cleanly revert temporary stat gains.
void player_reapply_stat_buffs(ActivePlayer* player);
void player_award_xp(ActivePlayer* player, uint64_t xp_amount);
void player_award_gold(ActivePlayer* player, uint32_t amount);
void player_send_kill_reward(int client_fd, ActivePlayer* player, uint32_t xp, uint32_t gold);
void player_send_stats(int client_fd, ActivePlayer* player);

// _locked variants: caller already holds player->lock
void player_award_xp_locked(ActivePlayer* player, uint64_t xp_amount);
void player_award_gold_locked(ActivePlayer* player, uint32_t amount);
void player_send_kill_reward_locked(int client_fd, ActivePlayer* player, uint32_t xp, uint32_t gold);
void player_send_stats_locked(int client_fd, ActivePlayer* player);

#endif