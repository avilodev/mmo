/**
 * @file
 * Handle attack, ability, damage, projectile, and progression packets.
 *
 * One of the four domain dispatchers split out of network.c's single
 * seventy-case switch. See net_internal.h for how they fit together.
 */

#include "net_internal.h"
#include "ability_bar.h"
#include "combat_system.h"
#include "combat_render.h"
#include "inventory.h"
#include "npc_types.h"
#include "audio/audio.h"
#include "ui/quest_log.h"
#include <stdio.h>
#include <string.h>
#include <stddef.h>
#include <math.h>

/**
 * Dispatch one combat packet.
 *
 * @param type  Opcode from the packet header.
 * @param data  Buffer holding one complete packet.
 * @param length  Available packet length in bytes.
 * @return 1 when this domain handled the opcode, otherwise 0.
 */
int net_dispatch_combat(uint8_t type, const char* data, int length) {
    (void)data; (void)length;

    switch (type) {
        case PACKET_ATTACK_RESULT:
            if (length >= (int)sizeof(AttackResultPacket)) {
                AttackResultPacket* pkt = (AttackResultPacket*)data;
                if (g_current_game && g_current_game->playing) {
                    combat_on_attack_result(&g_current_game->playing->combat, pkt->result_code, 0.5f);
                }
            }
            break;

        case PACKET_CAST_START_V2:
            if (length >= (int)sizeof(CastStartV2Packet)) {
                CastStartV2Packet* pkt = (CastStartV2Packet*)data;

                uint32_t targets[MAX_COMBAT_TARGETS];
                for (int i = 0; i < pkt->target_count && i < MAX_COMBAT_TARGETS; i++) {
                    targets[i] = ntohl(pkt->target_ids[i]);
                }

                if (g_current_game && g_current_game->playing) {
                    combat_on_cast_start(&g_current_game->playing->combat,
                                        pkt->attack_type,
                                        pkt->cast_time,
                                        pkt->origin_x, pkt->origin_y,
                                        pkt->aim_x, pkt->aim_y,
                                        targets, pkt->target_count);
                }
            }
            break;

        case PACKET_DAMAGE_V2:
            if (length >= (int)sizeof(DamageV2Packet)) {
                DamageV2Packet* pkt = (DamageV2Packet*)data;
                uint32_t target = ntohl(pkt->target_id);
                uint32_t damage = ntohl(pkt->damage);
                uint32_t new_health = ntohl(pkt->target_new_health);

                if (g_current_game && g_current_game->playing) {
                    float tx = 0, ty = 0;

                    if (target == g_net.character_id) {
                        // Player is the target — update health bar
                        g_current_game->player.info.health = new_health;
                        tx = g_current_game->player.x;
                        ty = g_current_game->player.y;
                    } else {
                        for (int i = 0; i < g_current_game->playing->visible_npc_count; i++) {
                            if (g_current_game->playing->visible_npcs[i].npc_id == target) {
                                tx = g_current_game->playing->visible_npcs[i].pos_x;
                                ty = g_current_game->playing->visible_npcs[i].pos_y;
                                break;
                            }
                        }
                    }

                    if (damage == 0 && !pkt->is_kill) {
                        combat_on_damage(&g_current_game->playing->combat,
                                         target, -1, 0, 0, 0, tx, ty);
                    } else {
                        combat_on_damage(&g_current_game->playing->combat,
                                         target, (int)damage, pkt->is_crit, pkt->is_kill, 0, tx, ty);
                    }

                    if (pkt->is_kill) {
                        network_request_player_stats();
                        network_request_player_data_refresh();
                    }
                }
            }
            break;

        case PACKET_CAST_CANCEL:
            if (length >= (int)sizeof(CastCancelPacket)) {
                if (g_current_game && g_current_game->playing) {
                    combat_on_cast_cancel(&g_current_game->playing->combat);
                }
            }
            break;

        case PACKET_ABILITY_CAST_START:
            if (length >= (int)sizeof(AbilityCastStartPacket)) {
                AbilityCastStartPacket* pkt = (AbilityCastStartPacket*)data;
                if (g_current_game && g_current_game->playing) {
                    uint16_t ability_id = ntohs(pkt->ability_id);
                    ability_bar_on_cast_start(&g_current_game->playing->ability_bar,
                                              ability_id, pkt->cast_time);
                    NET_LOG("[NET] Ability cast start: id=%u cast_time=%.1f\n",
                           ability_id, pkt->cast_time);
                }
            }
            break;

        case PACKET_ABILITY_EFFECT:
            if (length >= (int)sizeof(AbilityEffectPacket)) {
                AbilityEffectPacket* pkt = (AbilityEffectPacket*)data;
                if (g_current_game && g_current_game->playing) {
                    uint16_t ability_id = ntohs(pkt->ability_id);
                    uint32_t target = ntohl(pkt->target_id);
                    int32_t damage = (int32_t)ntohl(pkt->damage);
                    int32_t healing = (int32_t)ntohl(pkt->healing);
                    int32_t new_health = (int32_t)ntohl(pkt->target_new_health);

                    ability_bar_on_cast_resolve(&g_current_game->playing->ability_bar, ability_id);

                    float tx = 0, ty = 0;

                    if (target == g_net.character_id) {
                        // Player is the target — update health bar
                        if (new_health >= 0) {
                            g_current_game->player.info.health = (uint32_t)new_health;
                        }
                        tx = g_current_game->player.x;
                        ty = g_current_game->player.y;
                    } else {
                        for (int i = 0; i < g_current_game->playing->visible_npc_count; i++) {
                            if (g_current_game->playing->visible_npcs[i].npc_id == target) {
                                tx = g_current_game->playing->visible_npcs[i].pos_x;
                                ty = g_current_game->playing->visible_npcs[i].pos_y;
                                break;
                            }
                        }
                    }

                    if (damage == 0 && healing == 0 && !pkt->is_kill) {
                        combat_on_damage(&g_current_game->playing->combat,
                                         target, -1, 0, 0, 0, tx, ty);
                    } else if (damage > 0) {
                        combat_on_damage(&g_current_game->playing->combat,
                                         target, damage, pkt->is_crit, pkt->is_kill, 0, tx, ty);
                    } else if (healing > 0) {
                        combat_on_damage(&g_current_game->playing->combat,
                                         target, healing, pkt->is_crit, 0, 1, tx, ty);

                        // Spawn a green pulse ring at the healed target's position
                        for (int v = 0; v < MAX_HEAL_VFXS; v++) {
                            if (!g_current_game->playing->heal_vfxs[v].active) {
                                g_current_game->playing->heal_vfxs[v].active     = 1;
                                g_current_game->playing->heal_vfxs[v].pos_x      = tx;
                                g_current_game->playing->heal_vfxs[v].pos_y      = ty;
                                g_current_game->playing->heal_vfxs[v].age        = 0.0f;
                                g_current_game->playing->heal_vfxs[v].duration   = 0.7f;
                                g_current_game->playing->heal_vfxs[v].max_radius = 55.0f;
                                break;
                            }
                        }
                    }

                    if (pkt->is_kill) {
                        network_request_player_stats();
                        network_request_player_data_refresh();
                    }

                    NET_LOG("[NET] Ability effect: id=%u target=%u dmg=%d heal=%d\n",
                           ability_id, target, damage, healing);
                }
            }
            break;

        case PACKET_ABILITY_CAST_CANCEL:
            if (length >= (int)sizeof(AbilityCastCancelPacket)) {
                AbilityCastCancelPacket* pkt = (AbilityCastCancelPacket*)data;
                if (g_current_game && g_current_game->playing) {
                    uint16_t ability_id = ntohs(pkt->ability_id);
                    ability_bar_on_cast_cancel(&g_current_game->playing->ability_bar, ability_id);
                    NET_LOG("[NET] Ability cast cancelled (id=%u reason=%u)\n",
                           ability_id, pkt->reason);
                }
            }
            break;

        case PACKET_MANA_UPDATE:
            if (length >= (int)sizeof(ManaUpdatePacket)) {
                ManaUpdatePacket* pkt = (ManaUpdatePacket*)data;
                if (g_current_game && g_current_game->playing) {
                    /* The pair generalised rather than multiplied: whichever pool the
                     * character's role selects travels on this update. */
                    int32_t resource = (int32_t)ntohl(pkt->mana);
                    int32_t max_resource = (int32_t)ntohl(pkt->max_mana);
                    ability_bar_on_resource_update(&g_current_game->playing->ability_bar,
                                                   resource, max_resource);
                }
            }
            break;

        case PACKET_ABILITY_DATA:
            if (length >= (int)sizeof(AbilityDataPacket) && g_current_game && g_current_game->playing) {
                AbilityDataPacket* pkt = (AbilityDataPacket*)data;
                uint8_t count = pkt->count;
                if (count > MAX_ABILITY_SLOTS) count = MAX_ABILITY_SLOTS;

                uint16_t  ids[MAX_ABILITY_SLOTS]       = {0};
                const char* names[MAX_ABILITY_SLOTS]   = {"","","","",""};
                float     cooldowns[MAX_ABILITY_SLOTS] = {0};
                float     cast_times[MAX_ABILITY_SLOTS]= {0};
                int       costs[MAX_ABILITY_SLOTS]     = {0};
                const char* images[MAX_ABILITY_SLOTS]  = {"","","","",""};

                static char name_bufs[MAX_ABILITY_SLOTS][24];
                static char image_bufs[MAX_ABILITY_SLOTS][32];
                for (int i = 0; i < count; i++) {
                    ids[i]        = ntohs(pkt->slots[i].id);
                    memcpy(name_bufs[i], pkt->slots[i].name, 23);
                    name_bufs[i][23] = '\0';
                    names[i]      = name_bufs[i];
                    cooldowns[i]  = pkt->slots[i].cooldown;
                    cast_times[i] = pkt->slots[i].cast_time;
                    costs[i]      = (int)pkt->slots[i].resource_cost;
                    memcpy(image_bufs[i], pkt->slots[i].image, 31);
                    image_bufs[i][31] = '\0';
                    images[i]     = image_bufs[i];
                }

                /* Each packet carries one form's bar. The server sends both on entry,
                 * so the client can render either without a round trip. */
                uint8_t form = pkt->form < FORM_COUNT ? pkt->form : FORM_HUMAN;
                ability_bar_set_form_abilities(&g_current_game->playing->ability_bar, form,
                                               ids, names, cooldowns, cast_times, costs,
                                               images, count);
                NET_LOG("[NET] Ability data received: %d slots for %s form\n",
                       count, form == FORM_ANIMAL ? "animal" : "human");
            }
            break;

        case PACKET_STATUS_EFFECT_APPLY:
            if (length >= (int)sizeof(StatusEffectApplyPacket)) {
                StatusEffectApplyPacket* pkt = (StatusEffectApplyPacket*)data;
                if (g_current_game && g_current_game->playing) {
                    ability_bar_on_effect_apply(&g_current_game->playing->ability_bar,
                                                pkt->effect_type,
                                                (int)ntohl(pkt->value),
                                                pkt->duration,
                                                ntohl(pkt->source_id));
                }
            }
            break;

        case PACKET_STATUS_EFFECT_REMOVE:
            if (length >= (int)sizeof(StatusEffectRemovePacket)) {
                StatusEffectRemovePacket* pkt = (StatusEffectRemovePacket*)data;
                if (g_current_game && g_current_game->playing) {
                    ability_bar_on_effect_remove(&g_current_game->playing->ability_bar,
                                                 pkt->effect_type);
                }
            }
            break;

        case PACKET_SPAWN_ZONE:
            if (length >= (int)sizeof(SpawnZonePacket)) {
                SpawnZonePacket* pkt = (SpawnZonePacket*)data;
                NET_LOG("[NET] Zone spawned: id=%u at (%.1f, %.1f) radius=%.1f dur=%.1f\n",
                    (unsigned int)ntohl(pkt->zone_id), pkt->pos_x, pkt->pos_y,
                    pkt->radius, pkt->duration);
                if (g_current_game && g_current_game->playing) {
                    for (int i = 0; i < MAX_ZONES; i++) {
                        if (!g_current_game->playing->zones[i].active) {
                            g_current_game->playing->zones[i].zone_id = ntohl(pkt->zone_id);
                            g_current_game->playing->zones[i].pos_x = pkt->pos_x;
                            g_current_game->playing->zones[i].pos_y = pkt->pos_y;
                            g_current_game->playing->zones[i].radius = pkt->radius;
                            g_current_game->playing->zones[i].duration = pkt->duration;
                            g_current_game->playing->zones[i].elapsed = 0.0f;
                            g_current_game->playing->zones[i].active = 1;
                            break;
                        }
                    }
                }
            }
            break;

        case PACKET_REMOVE_ZONE:
            if (length >= (int)sizeof(RemoveZonePacket)) {
                RemoveZonePacket* pkt = (RemoveZonePacket*)data;
                uint32_t remove_zid = ntohl(pkt->zone_id);
                NET_LOG("[NET] Zone removed: id=%u\n", (unsigned int)remove_zid);
                if (g_current_game && g_current_game->playing) {
                    for (int i = 0; i < MAX_ZONES; i++) {
                        if (g_current_game->playing->zones[i].active &&
                            g_current_game->playing->zones[i].zone_id == remove_zid) {
                            g_current_game->playing->zones[i].active = 0;
                            break;
                        }
                    }
                }
            }
            break;

        case PACKET_LEVEL_UP:
            if (length >= (int)sizeof(LevelUpPacket)) {
                LevelUpPacket* pkt = (LevelUpPacket*)data;
                if (g_current_game && g_current_game->playing) {
                    uint32_t new_level    = ntohl(pkt->new_level);
                    uint32_t new_hp       = ntohl(pkt->new_health);
                    uint32_t new_max_hp   = ntohl(pkt->new_max_health);
                    uint32_t new_mana     = ntohl(pkt->new_resource);
                    uint32_t new_max_mana = ntohl(pkt->new_max_resource);

                    g_current_game->player.info.level      = new_level;
                    g_current_game->player.info.health     = new_hp;
                    g_current_game->player.info.max_health = new_max_hp;

                    for (int s = 0; s < STAT_COUNT; s++) {
                        g_current_game->playing->player_stats[s] = (int32_t)ntohl(pkt->stats[s]);
                    }
                    g_current_game->playing->player_xp_for_next = mmo_ntohll(pkt->xp_for_next_level);

                    ability_bar_on_resource_update(&g_current_game->playing->ability_bar,
                                                   (int32_t)new_mana, (int32_t)new_max_mana);

                    g_current_game->playing->show_level_up      = 1;
                    g_current_game->playing->level_up_timer      = 3.0f;
                    g_current_game->playing->level_up_new_level  = (int)new_level;
                    audio_event_level_up();

                    NET_LOG("[NET] LEVEL UP! Now level %u (HP=%u/%u, Mana=%u/%u)\n",
                           new_level, new_hp, new_max_hp, new_mana, new_max_mana);
                    net_log_player_stats("[NET] Level-up stats");
                }
            }
            break;

        case PACKET_PLAYER_STATS:
            if (length >= (int)sizeof(PlayerStatsPacket)) {
                PlayerStatsPacket* pkt = (PlayerStatsPacket*)data;
                if (g_current_game && g_current_game->playing) {
                    for (int s = 0; s < STAT_COUNT; s++) {
                        g_current_game->playing->player_stats[s] = (int32_t)ntohl(pkt->stats[s]);
                    }
                    g_current_game->playing->player_move_speed     = pkt->move_speed;
                    g_current_game->playing->player_form           = pkt->form;
                    g_current_game->playing->player_resource_type  = pkt->resource_type;
                    g_current_game->playing->player_weapon_damage   = (int32_t)ntohl(pkt->weapon_damage);
                    g_current_game->playing->player_xp_for_next    = mmo_ntohll(pkt->xp_for_next_level);

                    int32_t max_hp       = (int32_t)ntohl(pkt->max_health);
                    int32_t max_mana     = (int32_t)ntohl(pkt->max_resource);
                    int32_t current_hp   = (int32_t)ntohl(pkt->current_health);
                    int32_t current_mana = (int32_t)ntohl(pkt->current_resource);

                    g_current_game->player.info.max_health = (uint32_t)max_hp;
                    if (current_hp > 0) {
                        g_current_game->player.info.health = (uint32_t)current_hp;
                    }

                    /* The bar's form and pool are authoritative from this packet, so a
                     * reconnect or a stat refresh restores the right one. */
                    AbilityBarState* bar = &g_current_game->playing->ability_bar;
                    bar->active_form   = pkt->form < FORM_COUNT ? pkt->form : FORM_HUMAN;
                    bar->resource_type = pkt->resource_type;
                    ability_bar_on_resource_update(bar, current_mana, max_mana);

                    if (g_current_game->playing->player_move_speed > 0.0f) {
                        g_current_game->player.speed = g_current_game->playing->player_move_speed;
                    }

                    net_log_player_stats("[NET] Stats received");
                    NET_LOG("[NET]   speed=%.0f wdmg=%d HP=%d/%d resource=%d/%d form=%s\n",
                           g_current_game->playing->player_move_speed,
                           g_current_game->playing->player_weapon_damage,
                           current_hp, max_hp, current_mana, max_mana,
                           pkt->form == FORM_ANIMAL ? "animal" : "human");
                }
            }
            break;

        case PACKET_KILL_REWARD:
            if (length >= (int)sizeof(KillRewardPacket)) {
                KillRewardPacket* pkt = (KillRewardPacket*)data;
                if (g_current_game && g_current_game->playing) {
                    uint32_t xp_gained    = ntohl(pkt->xp_gained);
                    uint64_t total_xp     = mmo_ntohll(pkt->total_xp);

                    /* Kills pay experience only. The enemy's coin value arrives
                     * as loot the player sells to a kingdom's NPCs. */
                    g_current_game->player.info.experience = total_xp;

                    // Find an empty reward notification slot and spawn the notification
                    for (int i = 0; i < MAX_REWARD_POPUPS; i++) {
                        if (!g_current_game->playing->reward_notifications[i].active) {
                            g_current_game->playing->reward_notifications[i].xp_gained       = xp_gained;
                            g_current_game->playing->reward_notifications[i].currency_gained = 0;
                            g_current_game->playing->reward_notifications[i].age             = 0.0f;
                            g_current_game->playing->reward_notifications[i].active          = 1;
                            break;
                        }
                    }

                    NET_LOG("[NET] Kill Reward: +%u XP (Total: %llu XP)\n",
                           xp_gained, (unsigned long long)total_xp);
                }
            }
            break;

        case PACKET_PROJECTILE_SPAWN:
            if (length >= (int)sizeof(ProjectileSpawnPacket)) {
                ProjectileSpawnPacket* pkt = (ProjectileSpawnPacket*)data;
                if (g_current_game && g_current_game->playing) {
                    for (int i = 0; i < MAX_VISIBLE_PROJECTILES; i++) {
                        if (!g_current_game->playing->projectiles[i].active) {
                            g_current_game->playing->projectiles[i].id = ntohl(pkt->projectile_id);
                            g_current_game->playing->projectiles[i].pos_x = pkt->pos_x;
                            g_current_game->playing->projectiles[i].pos_y = pkt->pos_y;
                            g_current_game->playing->projectiles[i].dir_x = pkt->dir_x;
                            g_current_game->playing->projectiles[i].dir_y = pkt->dir_y;
                            g_current_game->playing->projectiles[i].speed = pkt->speed;
                            g_current_game->playing->projectiles[i].active = 1;
                            break;
                        }
                    }
                }
            }
            break;

        case PACKET_PROJECTILE_UPDATE: {
            size_t base_size = offsetof(ProjectileUpdatePacket, projectiles);

            if (length < (int)base_size) {
                return 1;   // handled: the packet was rejected, not unrecognized
            }

            ProjectileUpdatePacket* pkt = (ProjectileUpdatePacket*)data;
            uint8_t claimed_count = pkt->count;

            size_t expected_size = base_size + (claimed_count * sizeof(ProjectilePositionData));

            if (length < (int)expected_size) {
                return 1;   // handled: the packet was rejected, not unrecognized
            }

            if (claimed_count > MAX_PROJECTILES_PER_PACKET) {
                claimed_count = MAX_PROJECTILES_PER_PACKET;
            }

            if (g_current_game && g_current_game->playing) {
                for (int p = 0; p < claimed_count; p++) {
                    uint32_t pid = ntohl(pkt->projectiles[p].projectile_id);
                    for (int i = 0; i < MAX_VISIBLE_PROJECTILES; i++) {
                        if (g_current_game->playing->projectiles[i].active &&
                            g_current_game->playing->projectiles[i].id == pid) {
                            g_current_game->playing->projectiles[i].pos_x = pkt->projectiles[p].pos_x;
                            g_current_game->playing->projectiles[i].pos_y = pkt->projectiles[p].pos_y;
                            break;
                        }
                    }
                }
            }
            break;
        }

        case PACKET_PROJECTILE_DESTROY:
            if (length >= (int)sizeof(ProjectileDestroyPacket)) {
                ProjectileDestroyPacket* pkt = (ProjectileDestroyPacket*)data;
                if (g_current_game && g_current_game->playing) {
                    uint32_t pid = ntohl(pkt->projectile_id);
                    for (int i = 0; i < MAX_VISIBLE_PROJECTILES; i++) {
                        if (g_current_game->playing->projectiles[i].active &&
                            g_current_game->playing->projectiles[i].id == pid) {
                            g_current_game->playing->projectiles[i].active = 0;
                            break;
                        }
                    }
                }
            }
            break;

        case PACKET_PLAYER_DEATH:
            if (length >= (int)sizeof(PlayerDeathPacket)) {
                PlayerDeathPacket* pkt = (PlayerDeathPacket*)data;
                uint32_t dead_id = ntohl(pkt->dead_player_id);
                NET_LOG("[NET] Player %u died (killer: %u)\n",
                       dead_id, (uint32_t)ntohl(pkt->killer_id));
                if (g_current_game && g_current_game->playing && dead_id == g_net.character_id) {
                    g_current_game->playing->is_dead = 1;
                    g_current_game->playing->death_timer = 0.0f;
                    g_current_game->player.info.health = 0;
                    audio_event_death();
                }
            }
            break;

        case PACKET_PLAYER_RESPAWN:
            if (length >= (int)sizeof(PlayerRespawnPacket)) {
                PlayerRespawnPacket* pkt = (PlayerRespawnPacket*)data;
                uint32_t respawn_id = ntohl(pkt->player_id);
                NET_LOG("[NET] Player %u respawned at (%.1f, %.1f)\n",
                       respawn_id, pkt->pos_x, pkt->pos_y);
                if (g_current_game && g_current_game->playing && respawn_id == g_net.character_id) {
                    g_current_game->playing->is_dead = 0;
                    g_current_game->playing->death_timer = 0.0f;
                    g_current_game->player.x = pkt->pos_x;
                    g_current_game->player.y = pkt->pos_y;
                    g_current_game->player.info.health = (uint32_t)ntohl(pkt->health);
                    g_current_game->player.info.max_health = (uint32_t)ntohl(pkt->max_health);
                    g_current_game->player.needs_position_reset = 1;
                    ability_bar_on_resource_update(&g_current_game->playing->ability_bar,
                                                   (int32_t)ntohl(pkt->mana),
                                                   (int32_t)ntohl(pkt->max_mana));
                }
            }
            break;

        case PACKET_NPC_TELEGRAPH_START:
            if (length >= (int)sizeof(NPCTelegraphStartPacket)) {
                NPCTelegraphStartPacket* pkt = (NPCTelegraphStartPacket*)data;
                NET_LOG("[NET] Telegraph start: npc %u, shape %u, cast %.1fs\n",
                       (uint32_t)ntohl(pkt->npc_id), pkt->shape, pkt->cast_time);
                if (g_current_game && g_current_game->playing) {
                    for (int i = 0; i < MAX_TELEGRAPHS; i++) {
                        if (!g_current_game->playing->telegraphs[i].active) {
                            g_current_game->playing->telegraphs[i].npc_id = ntohl(pkt->npc_id);
                            g_current_game->playing->telegraphs[i].shape = pkt->shape;
                            g_current_game->playing->telegraphs[i].pos_x = pkt->pos_x;
                            g_current_game->playing->telegraphs[i].pos_y = pkt->pos_y;
                            g_current_game->playing->telegraphs[i].dir_x = pkt->dir_x;
                            g_current_game->playing->telegraphs[i].dir_y = pkt->dir_y;
                            g_current_game->playing->telegraphs[i].radius = pkt->radius;
                            g_current_game->playing->telegraphs[i].angle = pkt->angle;
                            g_current_game->playing->telegraphs[i].width = pkt->width;
                            g_current_game->playing->telegraphs[i].length = pkt->length;
                            g_current_game->playing->telegraphs[i].cast_time = pkt->cast_time;
                            g_current_game->playing->telegraphs[i].elapsed = 0.0f;
                            g_current_game->playing->telegraphs[i].active = 1;
                            break;
                        }
                    }
                }
            }
            break;

        case PACKET_NPC_TELEGRAPH_RESOLVE:
            if (length >= (int)sizeof(NPCTelegraphResolvePacket)) {
                NPCTelegraphResolvePacket* pkt = (NPCTelegraphResolvePacket*)data;
                uint32_t resolve_npc = ntohl(pkt->npc_id);
                NET_LOG("[NET] Telegraph resolve: npc %u\n", resolve_npc);
                if (g_current_game && g_current_game->playing) {
                    for (int i = 0; i < MAX_TELEGRAPHS; i++) {
                        if (g_current_game->playing->telegraphs[i].active &&
                            g_current_game->playing->telegraphs[i].npc_id == resolve_npc) {
                            g_current_game->playing->telegraphs[i].active = 0;
                            break;
                        }
                    }
                }
            }
            break;

        default:
            return 0;   // not ours; the next domain gets a look
    }

    return 1;
}

/* --- Attack, ability, and stat requests ---------------------------------- */

/**
 * Send a basic-attack intent toward a world-space aim point.
 */
void network_send_attack_intent(float aim_x, float aim_y) {
    if (!g_net.connected) return;

    AttackIntentPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.header.type = PACKET_ATTACK_INTENT;
    pkt.header.player_id = htonl(g_net.character_id);
    pkt.header.payload_size = htons(sizeof(AttackIntentPacket) - sizeof(PacketHeader));
    pkt.aim_x = aim_x;
    pkt.aim_y = aim_y;

    if (net_send((char*)&pkt, sizeof(pkt)) == sizeof(pkt)) {
        NET_LOG("[NET] Attack intent sent: aim (%.1f, %.1f)\n", aim_x, aim_y);
    }
}

/**
 * Send an ability-cast intent with aim and optional target information.
 */
void network_send_ability_cast(uint16_t ability_id, float aim_x, float aim_y, uint32_t target_id) {
    if (!g_net.connected) return;

    AbilityCastIntentPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.header.type = PACKET_ABILITY_CAST_INTENT;
    pkt.header.player_id = htonl(g_net.character_id);
    pkt.header.payload_size = htons(sizeof(AbilityCastIntentPacket) - sizeof(PacketHeader));
    pkt.ability_id = htons(ability_id);
    pkt.aim_x = aim_x;
    pkt.aim_y = aim_y;
    pkt.target_id = htonl(target_id);

    if (net_send((char*)&pkt, sizeof(pkt)) == sizeof(pkt)) {
        NET_LOG("[NET] Ability cast intent sent: id=%u aim=(%.1f, %.1f)\n",
               ability_id, aim_x, aim_y);
    }
}

/**
 * Send a cancellation for the local character's active ability cast.
 */
void network_send_ability_cancel(void) {
    if (!g_net.connected) return;

    AbilityCastCancelPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.header.type = PACKET_ABILITY_CAST_CANCEL;
    pkt.header.player_id = htonl(g_net.character_id);
    pkt.header.payload_size = htons(sizeof(AbilityCastCancelPacket) - sizeof(PacketHeader));
    pkt.caster_id = htonl(g_net.character_id);
    pkt.ability_id = 0;
    pkt.reason = 0;

    net_send((char*)&pkt, sizeof(pkt));
}

/**
 * Request current server-authoritative combat statistics.
 */
void network_request_player_stats(void) {
    if (!g_net.connected) return;

    RequestPlayerStatsPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.header.type = PACKET_REQUEST_PLAYER_STATS;
    pkt.header.player_id = htonl(g_net.character_id);
    pkt.header.payload_size = 0; // Header-only packet

    net_send((char*)&pkt, sizeof(pkt));
    NET_LOG("[NET] Requested player stats refresh\n");
}

/**
 * Request current character data for the active character.
 */
void network_request_player_data_refresh(void) {
    if (!g_net.connected || g_net.character_id == 0) return;

    WorldPlayerDataRequest pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.header.type = PACKET_REQUEST_PLAYER_DATA;
    pkt.header.player_id = htonl(g_net.account_id);
    pkt.header.payload_size = htons(sizeof(WorldPlayerDataRequest) - sizeof(PacketHeader));
    pkt.character_id = htonl(g_net.character_id);
    pkt.world_id = 0;

    net_send((char*)&pkt, sizeof(pkt));
    NET_LOG("[NET] Requested player data refresh (XP/balances)\n");
}
