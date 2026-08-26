/**
 * @file
 * Derive player statistics, apply equipment and buffs, grant rewards, and send stat packets.
 */

#include "types.h"
#include "log.h"
#include "player_level.h"
#include "class_stats.h"
#include "player_effects.h"
#include "ability_handler.h"
#include "ability_def.h"
#include "items_database.h"
#include "combat_stats.h"
#include "utils.h"

#include <stdio.h>
#include <string.h>
#include <arpa/inet.h>
#include <sys/socket.h>

/**
 * Reset a player's derived statistics to race-and-level values.
 *
 * Kept as a name the rest of the server already calls; the work itself lives with
 * the effects that also contribute to a player's attributes.
 */
void player_apply_class_stats(ActivePlayer* player) {
    player_recompute_stats(player);
}

/**
 * Rebuild derived statistics from the race curve and equipped items.
 */
void player_apply_equipment_bonuses(ActivePlayer* player) {
    player_recompute_stats(player);
}

/**
 * Rebuild equipment statistics and reapply every active stat buff.
 */
void player_reapply_stat_buffs(ActivePlayer* player) {
    player_recompute_stats(player);
}

/**
 * Grant XP while locking the player and process any resulting level gain.
 */
void player_award_xp(ActivePlayer* player, uint64_t xp_amount) {
    if (!player || xp_amount == 0) return;

    pthread_mutex_lock(&player->lock);
    player_award_xp_locked(player, xp_amount);
    pthread_mutex_unlock(&player->lock);
}

/**
 * Grant XP to an already locked player and process any resulting level gain.
 *
 * The caller must hold player->lock.
 */
void player_award_xp_locked(ActivePlayer* player, uint64_t xp_amount) {
    if (!player || xp_amount == 0) return;

    int old_level = player->level;
    player->experience += xp_amount;
    player->is_dirty = 1;

    int new_level = class_stats_check_level(player->level, player->experience);
    if (new_level <= old_level) return;

    player->level = new_level;
    player_recompute_stats(player);

    /* Full restore on level-up. Rage is the exception: it builds through combat
     * rather than being granted, so a level-up leaves a rage bar empty. */
    player->health = player->max_health;
    player->resource = (player->resource_type == RESOURCE_RAGE) ? 0 : player->max_resource;

    LOG_INFO("[LEVEL] Player %u (%s) leveled up: %d -> %d (HP=%d, resource=%d)",
             player->character_id, player->username,
             old_level, new_level, player->max_health, player->max_resource);

    /* Both forms' bars are rebuilt: a level can unlock an ability in either. */
    ability_refresh_hotbars(player);

    int fd = player->client_fd;

    LevelUpPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.header.type         = PACKET_LEVEL_UP;
    pkt.header.player_id    = htonl(player->character_id);
    pkt.header.payload_size = htons(sizeof(LevelUpPacket) - sizeof(PacketHeader));
    pkt.new_level           = htonl(player->level);
    pkt.new_max_health      = htonl(player->max_health);
    pkt.new_max_resource    = htonl(player->max_resource);
    pkt.new_health          = htonl(player->health);
    pkt.new_resource        = htonl(player->resource);
    for (int stat = 0; stat < STAT_COUNT; stat++) {
        pkt.stats[stat] = htonl(player->stats[stat]);
    }
    pkt.xp_for_next_level   = mmo_htonll(class_stats_xp_for_level(player->level + 1));

    /* The lock is held; server_send is non-blocking, so sending under it is safe. */
    server_send(fd, &pkt, sizeof(pkt));
    ability_send_data(fd, player);
}

/**
 * Send a kill-reward packet after snapshotting totals under the player lock.
 */
void player_send_kill_reward(int client_fd, ActivePlayer* player, uint32_t xp) {
    if (!player) return;

    pthread_mutex_lock(&player->lock);

    KillRewardPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.header.type         = PACKET_KILL_REWARD;
    pkt.header.player_id    = htonl(player->character_id);
    pkt.header.payload_size = htons(sizeof(KillRewardPacket) - sizeof(PacketHeader));
    pkt.xp_gained           = htonl(xp);
    pkt.total_xp            = mmo_htonll(player->experience);

    pthread_mutex_unlock(&player->lock);
    server_send(client_fd, &pkt, sizeof(pkt));
}

/**
 * Send a kill-reward packet for an already locked player.
 *
 * The caller must hold player->lock.
 */
void player_send_kill_reward_locked(int client_fd, ActivePlayer* player, uint32_t xp) {
    if (!player) return;

    KillRewardPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.header.type         = PACKET_KILL_REWARD;
    pkt.header.player_id    = htonl(player->character_id);
    pkt.header.payload_size = htons(sizeof(KillRewardPacket) - sizeof(PacketHeader));
    pkt.xp_gained           = htonl(xp);
    pkt.total_xp            = mmo_htonll(player->experience);

    server_send(client_fd, &pkt, sizeof(pkt));
}

/**
 * Fill a stat snapshot from an already locked player.
 *
 * The caller must hold player->lock.
 */
static void build_stats_packet(PlayerStatsPacket* pkt, const ActivePlayer* player) {
    memset(pkt, 0, sizeof(*pkt));
    pkt->header.type         = PACKET_PLAYER_STATS;
    pkt->header.player_id    = htonl(player->character_id);
    pkt->header.payload_size = htons(sizeof(PlayerStatsPacket) - sizeof(PacketHeader));

    for (int stat = 0; stat < STAT_COUNT; stat++) {
        pkt->stats[stat] = htonl(player->stats[stat]);
    }

    pkt->max_health       = htonl(player->max_health);
    pkt->max_resource     = htonl(player->max_resource);
    pkt->current_health   = htonl(player->health);
    pkt->current_resource = htonl(player->resource);
    pkt->move_speed       = player->move_speed;
    pkt->weapon_damage    = htonl(player->weapon_damage);
    pkt->resource_type    = player->resource_type;
    pkt->form             = player->form;
    pkt->xp_for_next_level = mmo_htonll(class_stats_xp_for_level(player->level + 1));
}

/**
 * Send a player's current derived statistics after locking it.
 */
void player_send_stats(int client_fd, ActivePlayer* player) {
    if (!player) return;

    PlayerStatsPacket pkt;
    pthread_mutex_lock(&player->lock);
    build_stats_packet(&pkt, player);
    pthread_mutex_unlock(&player->lock);

    server_send(client_fd, &pkt, sizeof(pkt));
}

/**
 * Send current derived statistics for an already locked player.
 *
 * The caller must hold player->lock.
 */
void player_send_stats_locked(int client_fd, ActivePlayer* player) {
    if (!player) return;

    PlayerStatsPacket pkt;
    build_stats_packet(&pkt, player);
    server_send(client_fd, &pkt, sizeof(pkt));
}
