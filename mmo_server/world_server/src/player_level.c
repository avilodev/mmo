#include "types.h"
#include "player_level.h"
#include "class_stats.h"
#include "ability_def.h"
#include "items_database.h"

#include <stdio.h>
#include <string.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#define VITALITY_HP_PER_POINT 5

void player_apply_class_stats(ActivePlayer* player) {
    if (!player) return;

    DerivedStats stats;
    if (!class_stats_compute(player->player_class, player->level, &stats)) {
        printf("[LEVEL] WARNING: Failed to compute stats for class %u level %d\n",
               player->player_class, player->level);
        return;
    }

    player->max_health   = stats.max_health;
    player->max_mana     = stats.max_mana;
    player->strength     = stats.strength;
    player->agility      = stats.agility;
    player->intelligence = stats.intelligence;
    player->wisdom       = stats.wisdom;
    player->defense      = stats.defense;
    player->evasion      = stats.evasion;
    player->vitality     = stats.vitality;
    player->luck         = stats.luck;
    player->move_speed   = stats.move_speed;
    player->weapon_damage = 0;

    if (player->health > player->max_health) player->health = player->max_health;
    if (player->mana > player->max_mana)     player->mana = player->max_mana;
}

void player_apply_equipment_bonuses(ActivePlayer* player) {
    if (!player) return;

    // Reset to class base first
    player_apply_class_stats(player);

    // Gather all 7 equipment slot IDs
    uint32_t equipped[] = {
        player->helmet, player->gloves, player->chest_armor,
        player->leggings, player->boots, player->main_hand,
        player->second_hand
    };

    for (int i = 0; i < 7; i++) {
        if (equipped[i] == 0) continue;
        const ItemDefinition* item = item_get(equipped[i]);
        if (!item) continue;

        // Weapon damage stacks (main_hand + off_hand)
        if (item->type == ITEM_TYPE_WEAPON) {
            player->weapon_damage += (int)item->damage;
        }

        // Armor/shield base defense goes into defense stat
        if (item->type == ITEM_TYPE_ARMOR || item->type == ITEM_TYPE_SHIELD) {
            player->defense += (int)item->defense;
        }

        // Stat bonuses from any equipment
        player->strength     += item->bonus_strength;
        player->agility      += item->bonus_agility;
        player->intelligence += item->bonus_intelligence;
        player->wisdom       += item->bonus_wisdom;
        player->defense      += item->bonus_defense;
        player->evasion      += item->bonus_evasion;
        player->vitality     += item->bonus_vitality;
        player->luck         += item->bonus_luck;
    }

    // Vitality increases max HP
    player->max_health += player->vitality * VITALITY_HP_PER_POINT;

    // Clamp current values
    if (player->health > player->max_health) player->health = player->max_health;
    if (player->mana > player->max_mana)     player->mana = player->max_mana;
}

void player_award_xp(ActivePlayer* player, uint64_t xp_amount) {
    if (!player || xp_amount == 0) return;

    pthread_mutex_lock(&player->lock);

    int old_level = player->level;
    player->experience += xp_amount;
    player->is_dirty = 1;

    int new_level = class_stats_check_level(player->level, player->experience);

    if (new_level > old_level) {
        player->level = new_level;
        player_apply_equipment_bonuses(player);

        // Full heal on level-up
        player->health = player->max_health;
        player->mana   = player->max_mana;

        printf("[LEVEL] Player %u (%s) leveled up: %d -> %d (HP=%d, Mana=%d)\n",
               player->character_id, player->username,
               old_level, new_level,
               player->max_health, player->max_mana);

        // Reassign abilities for new level
        {
            uint16_t class_abilities[10];
            int total = ability_get_class_abilities(player->player_class,
                                                     class_abilities, 10);

            player->ability_count = 0;
            memset(player->ability_cooldowns, 0, sizeof(player->ability_cooldowns));

            for (int a = 0; a < total && player->ability_count < 5; a++) {
                const AbilityDef* ab = ability_get(class_abilities[a]);
                if (ab && player->level >= ab->unlock_level) {
                    player->ability_slots[player->ability_count] = ab->id;
                    player->ability_cooldowns[player->ability_count] = 0.0f;
                    player->ability_count++;
                }
            }

            printf("[LEVEL] Player %u abilities reassigned: %d abilities at level %d\n",
                   player->character_id, player->ability_count, player->level);
        }

        int fd = player->client_fd;

        LevelUpPacket pkt;
        memset(&pkt, 0, sizeof(pkt));
        pkt.header.type         = PACKET_LEVEL_UP;
        pkt.header.player_id    = htonl(player->character_id);
        pkt.header.payload_size = htons(sizeof(LevelUpPacket) - sizeof(PacketHeader));
        pkt.new_level           = htonl(player->level);
        pkt.new_max_health      = htonl(player->max_health);
        pkt.new_max_mana        = htonl(player->max_mana);
        pkt.new_health          = htonl(player->health);
        pkt.new_mana            = htonl(player->mana);
        pkt.strength            = htonl(player->strength);
        pkt.agility             = htonl(player->agility);
        pkt.intelligence        = htonl(player->intelligence);
        pkt.wisdom              = htonl(player->wisdom);
        pkt.defense             = htonl(player->defense);
        pkt.evasion             = htonl(player->evasion);
        pkt.vitality            = htonl(player->vitality);
        pkt.luck                = htonl(player->luck);
        pkt.xp_for_next_level   = htonll(class_stats_xp_for_level(player->level + 1));

        pthread_mutex_unlock(&player->lock);
        send(fd, &pkt, sizeof(pkt), 0);
    } else {
        pthread_mutex_unlock(&player->lock);
    }
}

void player_send_stats(int client_fd, ActivePlayer* player) {
    if (!player) return;

    pthread_mutex_lock(&player->lock);

    PlayerStatsPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.header.type         = PACKET_PLAYER_STATS;
    pkt.header.player_id    = htonl(player->character_id);
    pkt.header.payload_size = htons(sizeof(PlayerStatsPacket) - sizeof(PacketHeader));
    pkt.strength            = htonl(player->strength);
    pkt.agility             = htonl(player->agility);
    pkt.intelligence        = htonl(player->intelligence);
    pkt.wisdom              = htonl(player->wisdom);
    pkt.defense             = htonl(player->defense);
    pkt.evasion             = htonl(player->evasion);
    pkt.vitality            = htonl(player->vitality);
    pkt.luck                = htonl(player->luck);
    pkt.max_health          = htonl(player->max_health);
    pkt.max_mana            = htonl(player->max_mana);
    pkt.current_health      = htonl(player->health);
    pkt.current_mana        = htonl(player->mana);
    pkt.move_speed          = player->move_speed;
    pkt.weapon_damage       = htonl(player->weapon_damage);
    pkt.xp_for_next_level   = htonll(class_stats_xp_for_level(player->level + 1));

    pthread_mutex_unlock(&player->lock);

    send(client_fd, &pkt, sizeof(pkt), 0);
}