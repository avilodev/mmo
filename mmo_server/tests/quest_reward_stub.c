// Stand in for the XP path a quest reward takes, and count what it was asked to pay.

#include "player_level.h"

#include <stdint.h>

// total XP quests have asked to award since the test started
uint64_t g_stub_xp_total;

// quest XP awards, and the zero-XP bar updates that follow each one
int g_stub_xp_awards;
int g_stub_bar_updates;

void player_award_xp_locked(ActivePlayer* player, uint64_t xp_amount) {
	(void)player;
	g_stub_xp_total += xp_amount;
	g_stub_xp_awards++;
}

void player_send_kill_reward_locked(int client_fd, ActivePlayer* player, uint32_t xp) {
	(void)client_fd;
	(void)player;
	(void)xp;
	g_stub_bar_updates++;
}
