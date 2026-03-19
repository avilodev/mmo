#include "ability_bar.h"
#include "network.h"
#include "game_types.h"
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
#include <GLFW/glfw3.h>

// Set to 1 to enable verbose per-packet logging (very noisy — disable in production)
#define NET_DEBUG 0
#if NET_DEBUG
#define NET_LOG(...) printf(__VA_ARGS__)
#else
#define NET_LOG(...) ((void)0)
#endif

// ============================================================================
// INTERNAL STATE
// ============================================================================

static uint32_t g_account_id = 0;
static uint32_t g_character_id = 0;
static BOOL g_initialized = FALSE;
static SOCKET g_socket = INVALID_SOCKET;
static BOOL g_connected = FALSE;

// Response storage
static WorldListResponsePacket g_world_list;
static BOOL g_world_list_ready = FALSE;

static CharacterListResponsePacket g_char_list;
static BOOL g_char_list_ready = FALSE;

static CharacterCreateResponsePacket g_char_create;
static BOOL g_char_create_ready = FALSE;

static CharacterDeleteResponsePacket g_char_delete;
static BOOL g_char_delete_ready = FALSE;

static EnterWorldResponsePacket g_enter_world;
static BOOL g_enter_world_ready = FALSE;

static CharacterInfo g_char_data;
static BOOL g_char_data_ready = FALSE;

static NPCInteractResponsePacket g_npc_interact_response;
static BOOL g_npc_interact_response_ready = FALSE;

static DialogueUpdatePacket g_dialogue_update;
static BOOL g_dialogue_update_ready = FALSE;

static DialogueClosePacket g_dialogue_close;
static BOOL g_dialogue_close_ready = FALSE;

static BOOL g_world_connect_ack_ready = FALSE;
static WorldConnectAckPacket g_world_connect_ack;

static BOOL g_realm_connect_ack_ready = FALSE;
static RealmConnectAckPacket g_realm_connect_ack;

static float g_correction_x = 0.0f;
static float g_correction_y = 0.0f;
static BOOL g_correction_ready = FALSE;

static float g_last_facing_angle = 0.0f;

// Ping tracking
#define PING_INTERVAL    10.0
#define PING_MAX_MISSED  3      // Disconnect after 3 unanswered pings (~30s)
static double g_last_ping_time  = 0.0;
static int    g_pending_pings   = 0;   // Sent but not yet echoed back
static double g_ping_send_time  = 0.0; // glfwGetTime() when last ping was sent
static int    g_ping_ms         = 0;   // Most recent measured RTT in milliseconds

// Stream reassembly buffer for TCP framing
static char g_recv_buf[65536];
static int  g_recv_len = 0;

// External game state for combat events
extern GameState* g_current_game;

// ============================================================================
// HELPERS
// ============================================================================

static double get_time(void) {
    return glfwGetTime();
}

// ============================================================================
// PACKET PROCESSOR
// ============================================================================

static void process_packet(const char* data, int length) {
    if (length < (int)sizeof(PacketHeader)) {
        printf("[NET] ⚠️ Packet too small: %d bytes (need at least %zu)\n", 
               length, sizeof(PacketHeader));
        return;
    }
    
    PacketHeader* header = (PacketHeader*)data;
    
    switch (header->type) {
        case PACKET_PING:
            // Server echoed our ping back — measure RTT
            if (g_pending_pings > 0) {
                g_pending_pings--;
                double rtt = (get_time() - g_ping_send_time) * 1000.0;
                if (rtt > 0.0 && rtt < 60000.0)
                    g_ping_ms = (int)rtt;
            }
            break;
            
        case PACKET_WORLD_LIST_RESPONSE: {
            size_t base_size = offsetof(WorldListResponsePacket, worlds);
            
            if (length < (int)base_size) {
                printf("[NET] ❌ WORLD_LIST packet too small: %d < %zu\n", length, base_size);
                return;
            }
            
            WorldListResponsePacket* pkt = (WorldListResponsePacket*)data;
            uint8_t claimed_count = pkt->count;
            
            if (length < (int)sizeof(WorldListResponsePacket)) {
                printf("[NET] WORLD_LIST incomplete: have %d bytes, need %zu\n",
                       length, sizeof(WorldListResponsePacket));
                return;
            }

            // Validate count is reasonable
            if (claimed_count > MAX_WORLDS) {
                printf("[NET] Server claims %d worlds, clamping to MAX_WORLDS=%d\n",
                       claimed_count, MAX_WORLDS);
                claimed_count = MAX_WORLDS;
            }

            memcpy(&g_world_list, data, sizeof(WorldListResponsePacket));
            g_world_list.count = claimed_count;
            
            for (int i = 0; i < claimed_count; i++) {
                g_world_list.worlds[i].name[63] = '\0';
                g_world_list.worlds[i].ip[15] = '\0';
                g_world_list.worlds[i].region[31] = '\0';
                
                // Debug: Print each world
                printf("[NET]   World %d: '%s' at %s:%d (status=%d)\n",
                       i, 
                       g_world_list.worlds[i].name,
                       g_world_list.worlds[i].ip,
                       ntohs(g_world_list.worlds[i].port),
                       g_world_list.worlds[i].status);
            }
            
            g_world_list_ready = TRUE;
            printf("[NET] ✓ World list received: %d worlds validated\n", claimed_count);
            break;
        }
            
        case PACKET_CHARACTER_LIST_RESPONSE: {
            size_t base_size = offsetof(CharacterListResponsePacket, characters);
            
            if (length < (int)base_size) {
                printf("[NET] ❌ CHAR_LIST packet too small: %d < %zu\n", length, base_size);
                return;
            }
            
            CharacterListResponsePacket* pkt = (CharacterListResponsePacket*)data;
            uint8_t claimed_count = pkt->count;

            if (claimed_count > 10) {
                printf("[NET] Server claims %d characters, clamping to 10\n", claimed_count);
                claimed_count = 10;
            }

            size_t needed = base_size + (size_t)claimed_count * sizeof(pkt->characters[0]);
            if (length < (int)needed) {
                printf("[NET] CHAR_LIST incomplete: have %d bytes, need %zu\n",
                       length, needed);
                return;
            }

            memset(&g_char_list, 0, sizeof(g_char_list));
            memcpy(&g_char_list, data, needed);
            g_char_list.count = claimed_count;
            
            // Null-terminate character names
            for (int i = 0; i < claimed_count; i++) {
                g_char_list.characters[i].name[31] = '\0';
            }
            
            g_char_list_ready = TRUE;
            printf("[NET] ✓ Character list received: %d chars validated\n", claimed_count);
            break;
        }
            
        case PACKET_CHARACTER_CREATE_RESPONSE:
            if (length >= (int)sizeof(CharacterCreateResponsePacket)) {
                memcpy(&g_char_create, data, sizeof(CharacterCreateResponsePacket));
                g_char_create.character_name[31] = '\0';
                g_char_create.message[127] = '\0';
                g_char_create_ready = TRUE;
            }
            break;

        case PACKET_CHARACTER_DELETE_RESPONSE:
            if (length >= (int)sizeof(CharacterDeleteResponsePacket)) {
                memcpy(&g_char_delete, data, sizeof(CharacterDeleteResponsePacket));
                g_char_delete.message[127] = '\0';
                g_char_delete_ready = TRUE;
            }
            break;

        case PACKET_ENTER_WORLD_RESPONSE:
            if (length >= (int)sizeof(EnterWorldResponsePacket)) {
                memcpy(&g_enter_world, data, sizeof(EnterWorldResponsePacket));
                g_enter_world.game_ticket[63] = '\0';
                g_enter_world.world_ip[15] = '\0';
                g_enter_world.message[127] = '\0';
                g_enter_world_ready = TRUE;
            }
            break;

        case PACKET_REALM_CONNECT_ACK:
            if (length >= (int)sizeof(RealmConnectAckPacket)) {
                memcpy(&g_realm_connect_ack, data, sizeof(RealmConnectAckPacket));
                g_realm_connect_ack.message[127] = '\0';
                g_realm_connect_ack_ready = TRUE;
                printf("[NET] Realm connect ACK received: success=%d\n", g_realm_connect_ack.success);
            }
            break;
            
        case PACKET_WORLD_CONNECT_ACK:
            if (length >= (int)sizeof(WorldConnectAckPacket)) {
                memcpy(&g_world_connect_ack, data, sizeof(WorldConnectAckPacket));
                g_world_connect_ack.welcome_message[127] = '\0';
                g_world_connect_ack_ready = TRUE;
                printf("[NET] World connect ACK received\n");
            }
            break;
            
        case PACKET_PLAYER_DATA_RESPONSE:
            if (length >= (int)sizeof(CharacterInfo)) {
                memcpy(&g_char_data, data, sizeof(CharacterInfo));
                g_char_data.name[31] = '\0';
                g_char_data_ready = TRUE;

                // If game is already running, update live XP and gold immediately
                if (g_current_game && g_current_game->player.info_loaded) {
                    g_current_game->player.info.experience = ntohll(g_char_data.experience);
                    g_current_game->player.info.gold = ntohl(g_char_data.gold);
                }

                printf("[NET] Character data received\n");
            }
            break;
            
        case PACKET_PLAYER_MOVE_ACK:
            if (length >= (int)sizeof(PlayerMoveAckPacket)) {
                PlayerMoveAckPacket* ack = (PlayerMoveAckPacket*)data;
                g_correction_x = ack->pos_x;
                g_correction_y = ack->pos_y;
                g_correction_ready = TRUE;
            }
            break;
            
        case PACKET_ATTACK_RESULT:
            if (length >= (int)sizeof(AttackResultPacket)) {
                AttackResultPacket* pkt = (AttackResultPacket*)data;
                if (g_current_game) {
                    combat_on_attack_result(&g_current_game->combat, pkt->result_code, 0.5f);
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
                
                if (g_current_game) {
                    combat_on_cast_start(&g_current_game->combat,
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

                if (g_current_game) {
                    float tx = 0, ty = 0;

                    if (target == g_character_id) {
                        // Player is the target — update health bar
                        g_current_game->player.info.health = new_health;
                        tx = g_current_game->player.x;
                        ty = g_current_game->player.y;
                    } else {
                        for (int i = 0; i < g_current_game->visible_npc_count; i++) {
                            if (g_current_game->visible_npcs[i].npc_id == target) {
                                tx = g_current_game->visible_npcs[i].pos_x;
                                ty = g_current_game->visible_npcs[i].pos_y;
                                break;
                            }
                        }
                    }

                    if (damage == 0 && !pkt->is_kill) {
                        combat_on_damage(&g_current_game->combat,
                                         target, -1, 0, 0, 0, tx, ty);
                    } else {
                        combat_on_damage(&g_current_game->combat,
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
                if (g_current_game) {
                    combat_on_cast_cancel(&g_current_game->combat);
                }
            }
            break;

        case PACKET_ABILITY_CAST_START:
            if (length >= (int)sizeof(AbilityCastStartPacket)) {
                AbilityCastStartPacket* pkt = (AbilityCastStartPacket*)data;
                if (g_current_game) {
                    uint16_t ability_id = ntohs(pkt->ability_id);
                    ability_bar_on_cast_start(&g_current_game->ability_bar,
                                              ability_id, pkt->cast_time);
                    printf("[NET] Ability cast start: id=%u cast_time=%.1f\n",
                           ability_id, pkt->cast_time);
                }
            }
            break;

        case PACKET_ABILITY_EFFECT:
            if (length >= (int)sizeof(AbilityEffectPacket)) {
                AbilityEffectPacket* pkt = (AbilityEffectPacket*)data;
                if (g_current_game) {
                    uint16_t ability_id = ntohs(pkt->ability_id);
                    uint32_t target = ntohl(pkt->target_id);
                    int32_t damage = (int32_t)ntohl(pkt->damage);
                    int32_t healing = (int32_t)ntohl(pkt->healing);
                    int32_t new_health = (int32_t)ntohl(pkt->target_new_health);

                    ability_bar_on_cast_resolve(&g_current_game->ability_bar, ability_id);

                    float tx = 0, ty = 0;

                    if (target == g_character_id) {
                        // Player is the target — update health bar
                        if (new_health >= 0) {
                            g_current_game->player.info.health = (uint32_t)new_health;
                        }
                        tx = g_current_game->player.x;
                        ty = g_current_game->player.y;
                    } else {
                        for (int i = 0; i < g_current_game->visible_npc_count; i++) {
                            if (g_current_game->visible_npcs[i].npc_id == target) {
                                tx = g_current_game->visible_npcs[i].pos_x;
                                ty = g_current_game->visible_npcs[i].pos_y;
                                break;
                            }
                        }
                    }

                    if (damage == 0 && healing == 0 && !pkt->is_kill) {
                        combat_on_damage(&g_current_game->combat,
                                         target, -1, 0, 0, 0, tx, ty);
                    } else if (damage > 0) {
                        combat_on_damage(&g_current_game->combat,
                                         target, damage, pkt->is_crit, pkt->is_kill, 0, tx, ty);
                    } else if (healing > 0) {
                        combat_on_damage(&g_current_game->combat,
                                         target, healing, pkt->is_crit, 0, 1, tx, ty);

                        // Spawn a green pulse ring at the healed target's position
                        for (int v = 0; v < MAX_HEAL_VFXS; v++) {
                            if (!g_current_game->heal_vfxs[v].active) {
                                g_current_game->heal_vfxs[v].active     = 1;
                                g_current_game->heal_vfxs[v].pos_x      = tx;
                                g_current_game->heal_vfxs[v].pos_y      = ty;
                                g_current_game->heal_vfxs[v].age        = 0.0f;
                                g_current_game->heal_vfxs[v].duration   = 0.7f;
                                g_current_game->heal_vfxs[v].max_radius = 55.0f;
                                break;
                            }
                        }
                    }

                    if (pkt->is_kill) {
                        network_request_player_stats();
                        network_request_player_data_refresh();
                    }

                    printf("[NET] Ability effect: id=%u target=%u dmg=%d heal=%d\n",
                           ability_id, target, damage, healing);
                }
            }
            break;

        case PACKET_ABILITY_CAST_CANCEL:
            if (length >= (int)sizeof(AbilityCastCancelPacket)) {
                AbilityCastCancelPacket* pkt = (AbilityCastCancelPacket*)data;
                if (g_current_game) {
                    uint16_t ability_id = ntohs(pkt->ability_id);
                    ability_bar_on_cast_cancel(&g_current_game->ability_bar, ability_id);
                    printf("[NET] Ability cast cancelled (id=%u reason=%u)\n",
                           ability_id, pkt->reason);
                }
            }
            break;

        case PACKET_MANA_UPDATE:
            if (length >= (int)sizeof(ManaUpdatePacket)) {
                ManaUpdatePacket* pkt = (ManaUpdatePacket*)data;
                if (g_current_game) {
                    int32_t mana = (int32_t)ntohl(pkt->mana);
                    int32_t max_mana = (int32_t)ntohl(pkt->max_mana);
                    ability_bar_on_mana_update(&g_current_game->ability_bar, mana, max_mana);
                }
            }
            break;

        case PACKET_ABILITY_DATA:
            if (length >= (int)sizeof(AbilityDataPacket) && g_current_game) {
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
                    costs[i]      = (int)pkt->slots[i].mana_cost;
                    memcpy(image_bufs[i], pkt->slots[i].image, 31);
                    image_bufs[i][31] = '\0';
                    images[i]     = image_bufs[i];
                }

                ability_bar_set_abilities(&g_current_game->ability_bar,
                                          ids, names, cooldowns, cast_times, costs, images, count);
                printf("[NET] Ability data received: %d slots\n", count);
            }
            break;

        case PACKET_STATUS_EFFECT_APPLY:
            if (length >= (int)sizeof(StatusEffectApplyPacket)) {
                StatusEffectApplyPacket* pkt = (StatusEffectApplyPacket*)data;
                if (g_current_game) {
                    ability_bar_on_effect_apply(&g_current_game->ability_bar,
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
                if (g_current_game) {
                    ability_bar_on_effect_remove(&g_current_game->ability_bar,
                                                 pkt->effect_type);
                }
            }
            break;

        case PACKET_SPAWN_ZONE:
            if (length >= (int)sizeof(SpawnZonePacket)) {
                SpawnZonePacket* pkt = (SpawnZonePacket*)data;
                printf("[NET] Zone spawned: id=%u at (%.1f, %.1f) radius=%.1f dur=%.1f\n",
                    (unsigned int)ntohl(pkt->zone_id), pkt->pos_x, pkt->pos_y,
                    pkt->radius, pkt->duration);
                if (g_current_game) {
                    for (int i = 0; i < MAX_ZONES; i++) {
                        if (!g_current_game->zones[i].active) {
                            g_current_game->zones[i].zone_id = ntohl(pkt->zone_id);
                            g_current_game->zones[i].pos_x = pkt->pos_x;
                            g_current_game->zones[i].pos_y = pkt->pos_y;
                            g_current_game->zones[i].radius = pkt->radius;
                            g_current_game->zones[i].duration = pkt->duration;
                            g_current_game->zones[i].elapsed = 0.0f;
                            g_current_game->zones[i].active = 1;
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
                printf("[NET] Zone removed: id=%u\n", (unsigned int)remove_zid);
                if (g_current_game) {
                    for (int i = 0; i < MAX_ZONES; i++) {
                        if (g_current_game->zones[i].active &&
                            g_current_game->zones[i].zone_id == remove_zid) {
                            g_current_game->zones[i].active = 0;
                            break;
                        }
                    }
                }
            }
            break;
            
        case PACKET_NPC_POSITIONS: {
            size_t base_size = offsetof(NPCPositionPacket, npcs);
            
            if (length < (int)base_size) {
                printf("[NET] ❌ NPC_POSITIONS packet too small: %d < %zu\n", length, base_size);
                return;
            }
            
            NPCPositionPacket* pkt = (NPCPositionPacket*)data;
            uint8_t claimed_count = pkt->npc_count;
            
            size_t expected_size = base_size + (claimed_count * sizeof(NPCPositionData));
            
            if (length < (int)expected_size) {
                printf("[NET] ❌ NPC_POSITIONS incomplete: have %d bytes, need %zu for %d NPCs\n",
                       length, expected_size, claimed_count);
                return;
            }
            
            if (claimed_count > MAX_NPCS_PER_PACKET) {
                printf("[NET] ⚠️ Server claims %d NPCs, clamping to %d\n",
                       claimed_count, MAX_NPCS_PER_PACKET);
                claimed_count = MAX_NPCS_PER_PACKET;
            }
            
            printf("[NET] NPC positions received: %d NPCs\n", claimed_count);
            
            if (g_current_game) {
                int old_count = g_current_game->visible_npc_count;
                g_current_game->visible_npc_count = claimed_count;

                for (int i = 0; i < claimed_count && i < MAX_VISIBLE_NPCS; i++) {
                    NPCPositionData* src = &pkt->npcs[i];
                    uint32_t npc_id = ntohl(src->npc_id);
                    float new_x = src->pos_x;
                    float new_y = src->pos_y;

                    // Check if this NPC existed before to interpolate
                    int found = 0;
                    for (int j = 0; j < old_count && j < MAX_VISIBLE_NPCS; j++) {
                        if (g_current_game->visible_npcs[j].npc_id == npc_id) {
                            // Existing NPC: set up interpolation from current to new
                            g_current_game->visible_npcs[i].prev_x = g_current_game->visible_npcs[j].pos_x;
                            g_current_game->visible_npcs[i].prev_y = g_current_game->visible_npcs[j].pos_y;
                            found = 1;
                            break;
                        }
                    }

                    g_current_game->visible_npcs[i].npc_id = npc_id;
                    g_current_game->visible_npcs[i].target_x = new_x;
                    g_current_game->visible_npcs[i].target_y = new_y;

                    if (!found) {
                        // New NPC: snap to position immediately
                        g_current_game->visible_npcs[i].pos_x = new_x;
                        g_current_game->visible_npcs[i].pos_y = new_y;
                        g_current_game->visible_npcs[i].prev_x = new_x;
                        g_current_game->visible_npcs[i].prev_y = new_y;
                        g_current_game->visible_npcs[i].interp_t = 1.0f;
                    } else {
                        // Reset interpolation timer for smooth movement
                        g_current_game->visible_npcs[i].interp_t = 0.0f;
                    }

                    g_current_game->visible_npcs[i].health = ntohl(src->health);
                    g_current_game->visible_npcs[i].max_health = ntohl(src->max_health);
                    g_current_game->visible_npcs[i].is_alive = src->is_alive;
                    g_current_game->visible_npcs[i].category = src->category;
                    g_current_game->visible_npcs[i].is_interactable = src->is_interactable;
                    g_current_game->visible_npcs[i].npc_type_id = src->npc_type_id;
                    if (!found) {
                        const char* type_name = npc_type_get_name(src->npc_type_id);
                        if (type_name) {
                            snprintf(g_current_game->visible_npcs[i].name, 32, "%s", type_name);
                        } else {
                            snprintf(g_current_game->visible_npcs[i].name, 32, "NPC_%u", npc_id);
                        }
                    }
                }
            }
            break;
        }

        case PACKET_LEVEL_UP:
            if (length >= (int)sizeof(LevelUpPacket)) {
                LevelUpPacket* pkt = (LevelUpPacket*)data;
                if (g_current_game) {
                    uint32_t new_level    = ntohl(pkt->new_level);
                    uint32_t new_hp       = ntohl(pkt->new_health);
                    uint32_t new_max_hp   = ntohl(pkt->new_max_health);
                    uint32_t new_mana     = ntohl(pkt->new_mana);
                    uint32_t new_max_mana = ntohl(pkt->new_max_mana);

                    g_current_game->player.info.level      = new_level;
                    g_current_game->player.info.health     = new_hp;
                    g_current_game->player.info.max_health = new_max_hp;

                    g_current_game->player_strength      = (int32_t)ntohl(pkt->strength);
                    g_current_game->player_agility       = (int32_t)ntohl(pkt->agility);
                    g_current_game->player_intelligence   = (int32_t)ntohl(pkt->intelligence);
                    g_current_game->player_wisdom         = (int32_t)ntohl(pkt->wisdom);
                    g_current_game->player_defense        = (int32_t)ntohl(pkt->defense);
                    g_current_game->player_evasion        = (int32_t)ntohl(pkt->evasion);
                    g_current_game->player_vitality       = (int32_t)ntohl(pkt->vitality);
                    g_current_game->player_luck           = (int32_t)ntohl(pkt->luck);
                    g_current_game->player_xp_for_next    = ntohll(pkt->xp_for_next_level);

                    ability_bar_on_mana_update(&g_current_game->ability_bar,
                                               (int32_t)new_mana, (int32_t)new_max_mana);

                    g_current_game->show_level_up      = 1;
                    g_current_game->level_up_timer      = 3.0f;
                    g_current_game->level_up_new_level  = (int)new_level;
                    audio_event_level_up();

                    printf("[NET] LEVEL UP! Now level %u (HP=%u/%u, Mana=%u/%u)\n",
                           new_level, new_hp, new_max_hp, new_mana, new_max_mana);
                    printf("[NET] Stats: STR=%d AGI=%d INT=%d WIS=%d DEF=%d EVA=%d VIT=%d LCK=%d\n",
                           g_current_game->player_strength,
                           g_current_game->player_agility,
                           g_current_game->player_intelligence,
                           g_current_game->player_wisdom,
                           g_current_game->player_defense,
                           g_current_game->player_evasion,
                           g_current_game->player_vitality,
                           g_current_game->player_luck);
                }
            }
            break;

        case PACKET_PLAYER_STATS:
            if (length >= (int)sizeof(PlayerStatsPacket)) {
                PlayerStatsPacket* pkt = (PlayerStatsPacket*)data;
                if (g_current_game) {
                    g_current_game->player_strength      = (int32_t)ntohl(pkt->strength);
                    g_current_game->player_agility       = (int32_t)ntohl(pkt->agility);
                    g_current_game->player_intelligence   = (int32_t)ntohl(pkt->intelligence);
                    g_current_game->player_wisdom         = (int32_t)ntohl(pkt->wisdom);
                    g_current_game->player_defense        = (int32_t)ntohl(pkt->defense);
                    g_current_game->player_evasion        = (int32_t)ntohl(pkt->evasion);
                    g_current_game->player_vitality       = (int32_t)ntohl(pkt->vitality);
                    g_current_game->player_luck           = (int32_t)ntohl(pkt->luck);
                    g_current_game->player_move_speed     = pkt->move_speed;
                    g_current_game->player_weapon_damage   = (int32_t)ntohl(pkt->weapon_damage);
                    g_current_game->player_xp_for_next    = ntohll(pkt->xp_for_next_level);

                    int32_t max_hp       = (int32_t)ntohl(pkt->max_health);
                    int32_t max_mana     = (int32_t)ntohl(pkt->max_mana);
                    int32_t current_hp   = (int32_t)ntohl(pkt->current_health);
                    int32_t current_mana = (int32_t)ntohl(pkt->current_mana);

                    g_current_game->player.info.max_health = (uint32_t)max_hp;
                    if (current_hp > 0) {
                        g_current_game->player.info.health = (uint32_t)current_hp;
                    }

                    ability_bar_on_mana_update(&g_current_game->ability_bar,
                                               current_mana, max_mana);

                    if (g_current_game->player_move_speed > 0.0f) {
                        g_current_game->player.speed = g_current_game->player_move_speed;
                    }

                    printf("[NET] Stats received: STR=%d AGI=%d INT=%d WIS=%d DEF=%d EVA=%d VIT=%d LCK=%d speed=%.0f wdmg=%d HP=%d/%d Mana=%d/%d\n",
                           g_current_game->player_strength,
                           g_current_game->player_agility,
                           g_current_game->player_intelligence,
                           g_current_game->player_wisdom,
                           g_current_game->player_defense,
                           g_current_game->player_evasion,
                           g_current_game->player_vitality,
                           g_current_game->player_luck,
                           g_current_game->player_move_speed,
                           g_current_game->player_weapon_damage,
                           current_hp, max_hp,
                           current_mana, max_mana);
                }
            }
            break;

        case PACKET_KILL_REWARD:
            if (length >= (int)sizeof(KillRewardPacket)) {
                KillRewardPacket* pkt = (KillRewardPacket*)data;
                if (g_current_game) {
                    uint32_t xp_gained    = ntohl(pkt->xp_gained);
                    uint32_t gold_gained  = ntohl(pkt->gold_gained);
                    uint64_t total_xp     = ntohll(pkt->total_xp);
                    uint32_t total_gold   = ntohl(pkt->total_gold);

                    // Update player's gold and XP
                    g_current_game->player.info.gold       = total_gold;
                    g_current_game->player.info.experience = total_xp;

                    // Find an empty reward notification slot and spawn the notification
                    for (int i = 0; i < MAX_REWARD_POPUPS; i++) {
                        if (!g_current_game->reward_notifications[i].active) {
                            g_current_game->reward_notifications[i].xp_gained   = xp_gained;
                            g_current_game->reward_notifications[i].gold_gained = gold_gained;
                            g_current_game->reward_notifications[i].age         = 0.0f;
                            g_current_game->reward_notifications[i].active      = 1;
                            break;
                        }
                    }

                    printf("[NET] Kill Reward: +%u XP, +%u Gold (Total: %llu XP, %u Gold)\n",
                           xp_gained, gold_gained,
                           (unsigned long long)total_xp, total_gold);
                }
            }
            break;

        case PACKET_NPC_INTERACT_RESPONSE:
            if (length >= (int)sizeof(NPCInteractResponsePacket)) {
                memcpy(&g_npc_interact_response, data, sizeof(NPCInteractResponsePacket));
                g_npc_interact_response.npc_name[31] = '\0';
                g_npc_interact_response_ready = TRUE;
                printf("[NET] NPC Interact Response: NPC %u, Dialogue %u, Page %u\n",
                       (uint32_t)ntohl(g_npc_interact_response.npc_id),
                       (uint32_t)ntohl(g_npc_interact_response.dialogue_id),
                       g_npc_interact_response.page_num);
            }
            break;

        case PACKET_DIALOGUE_UPDATE:
            if (length >= (int)sizeof(DialogueUpdatePacket)) {
                memcpy(&g_dialogue_update, data, sizeof(DialogueUpdatePacket));
                g_dialogue_update_ready = TRUE;
                printf("[NET] Dialogue Update: Dialogue %u, Page %u\n",
                       (uint32_t)ntohl(g_dialogue_update.dialogue_id),
                       g_dialogue_update.page_num);
            }
            break;

        case PACKET_DIALOGUE_CLOSE:
            if (length >= (int)sizeof(DialogueClosePacket)) {
                memcpy(&g_dialogue_close, data, sizeof(DialogueClosePacket));
                g_dialogue_close_ready = TRUE;
                printf("[NET] Dialogue Close: NPC %u\n",
                       (uint32_t)ntohl(g_dialogue_close.npc_id));
            }
            break;

        case PACKET_EQUIP_ITEM_RESPONSE:
            if (length >= (int)sizeof(EquipItemResponsePacket)) {
                EquipItemResponsePacket* pkt = (EquipItemResponsePacket*)data;
                pkt->message[127] = '\0';
                printf("[NET] Equip response: %s - %s\n",
                       pkt->success ? "OK" : "FAIL", pkt->message);
                if (pkt->success && g_current_game) {
                    network_request_player_stats();
                }
            }
            break;

        case PACKET_UNEQUIP_ITEM_RESPONSE:
            if (length >= (int)sizeof(UnequipItemResponsePacket)) {
                UnequipItemResponsePacket* pkt = (UnequipItemResponsePacket*)data;
                pkt->message[127] = '\0';
                printf("[NET] Unequip response: %s - %s\n",
                       pkt->success ? "OK" : "FAIL", pkt->message);
                if (pkt->success && g_current_game) {
                    network_request_player_stats();
                }
            }
            break;

        case PACKET_MOVE_ITEM_RESPONSE:
            if (length >= (int)sizeof(MoveItemResponsePacket)) {
                MoveItemResponsePacket* pkt = (MoveItemResponsePacket*)data;
                printf("[NET] Move item response: %s (from=%u to=%u)\n",
                       pkt->success ? "OK" : "FAIL", pkt->from_slot, pkt->to_slot);
            }
            break;

        case PACKET_USE_ITEM_RESPONSE:
            if (length >= (int)sizeof(UseItemResponsePacket)) {
                UseItemResponsePacket* pkt = (UseItemResponsePacket*)data;
                pkt->message[127] = '\0';
                printf("[NET] Use item response: %s - %s\n",
                       pkt->success ? "OK" : "FAIL", pkt->message);
                if (pkt->success && g_current_game) {
                    int32_t new_hp = (int32_t)ntohl(pkt->new_health);
                    int32_t new_mana = (int32_t)ntohl(pkt->new_mana);
                    if (new_hp > 0) {
                        g_current_game->player.info.health = (uint32_t)new_hp;
                    }
                    if (new_mana >= 0) {
                        ability_bar_on_mana_update(&g_current_game->ability_bar,
                                                    new_mana,
                                                    g_current_game->ability_bar.max_mana);
                    }
                    // Remove consumed item from inventory
                    if (g_current_game->inventory) {
                        uint32_t used_item = ntohl(pkt->item_id);
                        for (int i = 0; i < INVENTORY_SIZE; i++) {
                            if (g_current_game->inventory->slots[i].template_id == used_item) {
                                inventory_remove_item(g_current_game->inventory, i, 1);
                                break;
                            }
                        }
                    }
                }
            }
            break;

        case PACKET_DROP_ITEM_RESPONSE:
            if (length >= (int)sizeof(DropItemResponsePacket)) {
                DropItemResponsePacket* pkt = (DropItemResponsePacket*)data;
                printf("[NET] Drop item response: %s\n",
                       pkt->success ? "OK" : "FAIL");
            }
            break;

        case PACKET_PLAYER_POSITIONS: {
            size_t base_size = offsetof(PlayerPositionBroadcastPacket, players);
            
            if (length < (int)base_size) {
                printf("[NET] ❌ PLAYER_POSITIONS packet too small: %d < %zu\n", length, base_size);
                return;
            }
            
            PlayerPositionBroadcastPacket* pkt = (PlayerPositionBroadcastPacket*)data;
            uint8_t claimed_count = pkt->count;
            
            size_t expected_size = base_size + (claimed_count * sizeof(NearbyPlayerData));
            
            if (length < (int)expected_size) {
                printf("[NET] ❌ PLAYER_POSITIONS incomplete: have %d bytes, need %zu for %d players\n",
                       length, expected_size, claimed_count);
                return;
            }
            
            if (claimed_count > MAX_NEARBY_PLAYERS) {
                printf("[NET] ⚠️ Server claims %d players, clamping to %d\n",
                       claimed_count, MAX_NEARBY_PLAYERS);
                claimed_count = MAX_NEARBY_PLAYERS;
            }
            
            if (g_current_game) {
                g_current_game->nearby_player_count = claimed_count;
                for (int i = 0; i < claimed_count; i++) {
                    NearbyPlayerData* src = &pkt->players[i];
                    g_current_game->nearby_players[i].player_id = ntohl(src->player_id);
                    g_current_game->nearby_players[i].pos_x = src->pos_x;
                    g_current_game->nearby_players[i].pos_y = src->pos_y;
                    g_current_game->nearby_players[i].health = (int32_t)ntohl(src->health);
                    g_current_game->nearby_players[i].max_health = (int32_t)ntohl(src->max_health);
                    g_current_game->nearby_players[i].player_class = src->player_class;
                    g_current_game->nearby_players[i].is_dead = src->is_dead;
                    g_current_game->nearby_players[i].ping_ms = ntohs(src->ping_ms);
                    snprintf(g_current_game->nearby_players[i].name, 32,
                             "Player_%u", (uint32_t)ntohl(src->player_id));
                }
            }
            break;
        }

        case PACKET_PROJECTILE_SPAWN:
            if (length >= (int)sizeof(ProjectileSpawnPacket)) {
                ProjectileSpawnPacket* pkt = (ProjectileSpawnPacket*)data;
                if (g_current_game) {
                    for (int i = 0; i < MAX_VISIBLE_PROJECTILES; i++) {
                        if (!g_current_game->projectiles[i].active) {
                            g_current_game->projectiles[i].id = ntohl(pkt->projectile_id);
                            g_current_game->projectiles[i].pos_x = pkt->pos_x;
                            g_current_game->projectiles[i].pos_y = pkt->pos_y;
                            g_current_game->projectiles[i].dir_x = pkt->dir_x;
                            g_current_game->projectiles[i].dir_y = pkt->dir_y;
                            g_current_game->projectiles[i].speed = pkt->speed;
                            g_current_game->projectiles[i].active = 1;
                            break;
                        }
                    }
                }
            }
            break;

        case PACKET_PROJECTILE_UPDATE: {
            size_t base_size = offsetof(ProjectileUpdatePacket, projectiles);
            
            if (length < (int)base_size) {
                return;
            }
            
            ProjectileUpdatePacket* pkt = (ProjectileUpdatePacket*)data;
            uint8_t claimed_count = pkt->count;
            
            size_t expected_size = base_size + (claimed_count * sizeof(ProjectilePositionData));
            
            if (length < (int)expected_size) {
                return;
            }
            
            if (claimed_count > MAX_PROJECTILES_PER_PACKET) {
                claimed_count = MAX_PROJECTILES_PER_PACKET;
            }
            
            if (g_current_game) {
                for (int p = 0; p < claimed_count; p++) {
                    uint32_t pid = ntohl(pkt->projectiles[p].projectile_id);
                    for (int i = 0; i < MAX_VISIBLE_PROJECTILES; i++) {
                        if (g_current_game->projectiles[i].active &&
                            g_current_game->projectiles[i].id == pid) {
                            g_current_game->projectiles[i].pos_x = pkt->projectiles[p].pos_x;
                            g_current_game->projectiles[i].pos_y = pkt->projectiles[p].pos_y;
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
                if (g_current_game) {
                    uint32_t pid = ntohl(pkt->projectile_id);
                    for (int i = 0; i < MAX_VISIBLE_PROJECTILES; i++) {
                        if (g_current_game->projectiles[i].active &&
                            g_current_game->projectiles[i].id == pid) {
                            g_current_game->projectiles[i].active = 0;
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
                printf("[NET] Player %u died (killer: %u)\n",
                       dead_id, (uint32_t)ntohl(pkt->killer_id));
                if (g_current_game && dead_id == g_character_id) {
                    g_current_game->is_dead = 1;
                    g_current_game->death_timer = 0.0f;
                    g_current_game->player.info.health = 0;
                    audio_event_death();
                }
            }
            break;

        case PACKET_PLAYER_RESPAWN:
            if (length >= (int)sizeof(PlayerRespawnPacket)) {
                PlayerRespawnPacket* pkt = (PlayerRespawnPacket*)data;
                uint32_t respawn_id = ntohl(pkt->player_id);
                printf("[NET] Player %u respawned at (%.1f, %.1f)\n",
                       respawn_id, pkt->pos_x, pkt->pos_y);
                if (g_current_game && respawn_id == g_character_id) {
                    g_current_game->is_dead = 0;
                    g_current_game->death_timer = 0.0f;
                    g_current_game->player.x = pkt->pos_x;
                    g_current_game->player.y = pkt->pos_y;
                    g_current_game->player.info.health = (uint32_t)ntohl(pkt->health);
                    g_current_game->player.info.max_health = (uint32_t)ntohl(pkt->max_health);
                    g_current_game->player.needs_position_reset = 1;
                    ability_bar_on_mana_update(&g_current_game->ability_bar,
                                               (int32_t)ntohl(pkt->mana),
                                               (int32_t)ntohl(pkt->max_mana));
                }
            }
            break;

        case PACKET_LOOT_DROP:
            if (length >= (int)sizeof(LootDropPacket)) {
                LootDropPacket* pkt = (LootDropPacket*)data;
                printf("[NET] Loot drop: ground_id=%u item=%u qty=%u at (%.1f, %.1f)\n",
                       (uint32_t)ntohl(pkt->ground_item_id), (uint32_t)ntohl(pkt->item_id),
                       pkt->quantity, pkt->pos_x, pkt->pos_y);
                if (g_current_game) {
                    for (int i = 0; i < MAX_GROUND_ITEMS; i++) {
                        if (!g_current_game->ground_items[i].active) {
                            g_current_game->ground_items[i].ground_item_id = ntohl(pkt->ground_item_id);
                            g_current_game->ground_items[i].item_id = ntohl(pkt->item_id);
                            g_current_game->ground_items[i].quantity = pkt->quantity;
                            g_current_game->ground_items[i].pos_x = pkt->pos_x;
                            g_current_game->ground_items[i].pos_y = pkt->pos_y;
                            g_current_game->ground_items[i].active = 1;
                            break;
                        }
                    }
                }
            }
            break;

        case PACKET_LOOT_PICKUP_RESPONSE:
            if (length >= (int)sizeof(LootPickupResponsePacket)) {
                LootPickupResponsePacket* pkt = (LootPickupResponsePacket*)data;
                pkt->message[63] = '\0';
                printf("[NET] Loot pickup: %s - %s\n",
                       pkt->success ? "OK" : "FAIL", pkt->message);
                if (pkt->success && g_current_game) {
                    audio_event_pickup();
                    uint32_t gid = ntohl(pkt->ground_item_id);
                    for (int i = 0; i < MAX_GROUND_ITEMS; i++) {
                        if (g_current_game->ground_items[i].active &&
                            g_current_game->ground_items[i].ground_item_id == gid) {
                            g_current_game->ground_items[i].active = 0;
                            break;
                        }
                    }
                    if (g_current_game->inventory) {
                        uint32_t item_id = ntohl(pkt->item_id);
                        uint8_t slot = pkt->inventory_slot;
                        if (slot < INVENTORY_SIZE) {
                            g_current_game->inventory->slots[slot].template_id = item_id;
                            g_current_game->inventory->slots[slot].quantity = pkt->quantity;
                        }
                    }
                }
            }
            break;

        case PACKET_LOOT_DESPAWN:
            if (length >= (int)sizeof(LootDespawnPacket)) {
                LootDespawnPacket* pkt = (LootDespawnPacket*)data;
                uint32_t despawn_id = ntohl(pkt->ground_item_id);
                printf("[NET] Loot despawned: %u\n", despawn_id);
                if (g_current_game) {
                    for (int i = 0; i < MAX_GROUND_ITEMS; i++) {
                        if (g_current_game->ground_items[i].active &&
                            g_current_game->ground_items[i].ground_item_id == despawn_id) {
                            g_current_game->ground_items[i].active = 0;
                            break;
                        }
                    }
                }
            }
            break;

        case PACKET_NPC_TELEGRAPH_START:
            if (length >= (int)sizeof(NPCTelegraphStartPacket)) {
                NPCTelegraphStartPacket* pkt = (NPCTelegraphStartPacket*)data;
                printf("[NET] Telegraph start: npc %u, shape %u, cast %.1fs\n",
                       (uint32_t)ntohl(pkt->npc_id), pkt->shape, pkt->cast_time);
                if (g_current_game) {
                    for (int i = 0; i < MAX_TELEGRAPHS; i++) {
                        if (!g_current_game->telegraphs[i].active) {
                            g_current_game->telegraphs[i].npc_id = ntohl(pkt->npc_id);
                            g_current_game->telegraphs[i].shape = pkt->shape;
                            g_current_game->telegraphs[i].pos_x = pkt->pos_x;
                            g_current_game->telegraphs[i].pos_y = pkt->pos_y;
                            g_current_game->telegraphs[i].dir_x = pkt->dir_x;
                            g_current_game->telegraphs[i].dir_y = pkt->dir_y;
                            g_current_game->telegraphs[i].radius = pkt->radius;
                            g_current_game->telegraphs[i].angle = pkt->angle;
                            g_current_game->telegraphs[i].width = pkt->width;
                            g_current_game->telegraphs[i].length = pkt->length;
                            g_current_game->telegraphs[i].cast_time = pkt->cast_time;
                            g_current_game->telegraphs[i].elapsed = 0.0f;
                            g_current_game->telegraphs[i].active = 1;
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
                printf("[NET] Telegraph resolve: npc %u\n", resolve_npc);
                if (g_current_game) {
                    for (int i = 0; i < MAX_TELEGRAPHS; i++) {
                        if (g_current_game->telegraphs[i].active &&
                            g_current_game->telegraphs[i].npc_id == resolve_npc) {
                            g_current_game->telegraphs[i].active = 0;
                            break;
                        }
                    }
                }
            }
            break;

        case PACKET_CHAT_MESSAGE:
            if (length >= (int)sizeof(ChatMessagePacket)) {
                ChatMessagePacket* pkt = (ChatMessagePacket*)data;
                pkt->sender_name[31] = '\0';
                pkt->message[MAX_CHAT_MESSAGE - 1] = '\0';
                printf("[CHAT] [ch%u] %s: %s\n",
                       pkt->channel, pkt->sender_name, pkt->message);
                if (g_current_game) {
                    ChatState* chat = &g_current_game->chat;
                    int idx = chat->line_count;
                    if (idx >= MAX_CHAT_LINES) {
                        memmove(&chat->lines[0], &chat->lines[1],
                                sizeof(ChatLine) * (MAX_CHAT_LINES - 1));
                        idx = MAX_CHAT_LINES - 1;
                    } else {
                        chat->line_count++;
                    }
                    memcpy(chat->lines[idx].sender, pkt->sender_name, 31);
                    chat->lines[idx].sender[31] = '\0';
                    memcpy(chat->lines[idx].text, pkt->message, 255);
                    chat->lines[idx].text[255] = '\0';
                    chat->lines[idx].channel = pkt->channel;

                    // Track whisper reply target: only set on received whispers,
                    // not on echo-backs (echo sender_name starts with "-> ")
                    if (pkt->channel == CHAT_CHANNEL_WHISPER &&
                        strncmp(pkt->sender_name, "-> ", 3) != 0) {
                        strncpy(chat->whisper_reply_target, pkt->sender_name,
                                sizeof(chat->whisper_reply_target) - 1);
                        chat->whisper_reply_target[sizeof(chat->whisper_reply_target) - 1] = '\0';
                    }
                }
            }
            break;

        case PACKET_PARTY_INVITE_NOTIFY:
            if (length >= (int)sizeof(PartyInviteNotifyPacket)) {
                PartyInviteNotifyPacket* pkt = (PartyInviteNotifyPacket*)data;
                pkt->from_name[31] = '\0';
                printf("[NET] Party invite from: %s\n", pkt->from_name);
                if (g_current_game) {
                    g_current_game->party.has_pending_invite = 1;
                    g_current_game->party.invite_from_id = ntohl(pkt->from_id);
                    strncpy(g_current_game->party.invite_from_name, pkt->from_name, 31);
                    g_current_game->party.invite_from_name[31] = '\0';
                    g_current_game->party.invite_timer = 30.0f;
                }
            }
            break;

        case PACKET_PARTY_UPDATE: {
            size_t base_size = offsetof(PartyUpdatePacket, members);
            
            if (length < (int)base_size) {
                return;
            }
            
            PartyUpdatePacket* pkt = (PartyUpdatePacket*)data;
            uint8_t claimed_count = pkt->member_count;
            
            size_t member_size = sizeof(pkt->members[0]);
            size_t expected_size = base_size + (claimed_count * member_size);
            
            if (length < (int)expected_size) {
                return;
            }
            
            if (claimed_count > MAX_PARTY_SIZE) {
                claimed_count = MAX_PARTY_SIZE;
            }
            
            printf("[NET] Party update: %u members\n", claimed_count);
            if (g_current_game) {
                PartyState* ps = &g_current_game->party;
                ps->party_id = ntohl(pkt->party_id);
                ps->leader_id = ntohl(pkt->leader_id);
                ps->member_count = claimed_count;
                ps->has_party = (claimed_count > 0) ? 1 : 0;
                ps->has_pending_invite = 0;
                for (int i = 0; i < claimed_count; i++) {
                    ps->members[i].id = ntohl(pkt->members[i].character_id);
                    strncpy(ps->members[i].name, pkt->members[i].name, 31);
                    ps->members[i].name[31] = '\0';
                    ps->members[i].level = pkt->members[i].level;
                    ps->members[i].player_class = pkt->members[i].player_class;
                    ps->members[i].health = (int32_t)ntohl(pkt->members[i].health);
                    ps->members[i].max_health = (int32_t)ntohl(pkt->members[i].max_health);
                    ps->members[i].mana = (int32_t)ntohl(pkt->members[i].mana);
                    ps->members[i].max_mana = (int32_t)ntohl(pkt->members[i].max_mana);
                }
            }
            break;
        }

        case PACKET_PARTY_DISBAND:
            printf("[NET] Party disbanded\n");
            if (g_current_game) {
                memset(&g_current_game->party, 0, sizeof(PartyState));
            }
            break;

        case PACKET_DISCONNECT:
            g_connected = FALSE;
            break;

        // ----------------------------------------------------------------
        // SHOP packets
        // ----------------------------------------------------------------

        case PACKET_SHOP_OPEN:
            if (length >= (int)sizeof(ShopOpenPacket) && g_current_game) {
                ShopOpenPacket* pkt = (ShopOpenPacket*)data;
                ShopState* shop = &g_current_game->shop;
                shop->shop_id = ntohl(pkt->shop_id);
                uint8_t count = pkt->item_count;
                if (count > MAX_SHOP_ITEMS) count = MAX_SHOP_ITEMS;
                shop->item_count = count;
                memcpy(shop->shop_name, pkt->shop_name, 31);
                shop->shop_name[31] = '\0';
                for (int i = 0; i < count; i++) {
                    shop->items[i].item_id   = ntohl(pkt->items[i].item_id);
                    shop->items[i].buy_price = ntohl(pkt->items[i].buy_price);
                }
                shop->sell_tab  = 0;
                shop->is_open   = 1;
                printf("[NET] Shop open: id=%u '%s' (%u items)\n",
                       shop->shop_id, shop->shop_name, count);
            }
            break;

        case PACKET_SHOP_BUY_RESPONSE:
            if (length >= (int)sizeof(ShopBuyResponsePacket) && g_current_game) {
                ShopBuyResponsePacket* pkt = (ShopBuyResponsePacket*)data;
                pkt->message[63] = '\0';
                printf("[NET] Shop buy: %s — %s\n",
                       pkt->success ? "OK" : "FAIL", pkt->message);
                if (pkt->success) {
                    // Update gold and inventory from server-authoritative values
                    g_current_game->player.info.gold = ntohl(pkt->new_gold);
                    uint8_t slot = pkt->inventory_slot;
                    if (slot < INVENTORY_SIZE && g_current_game->inventory) {
                        uint32_t item_id = ntohl(pkt->item_id);
                        g_current_game->inventory->slots[slot].template_id = item_id;
                        g_current_game->inventory->slots[slot].quantity     = 1;
                    }
                }
            }
            break;

        case PACKET_SHOP_SELL_RESPONSE:
            if (length >= (int)sizeof(ShopSellResponsePacket) && g_current_game) {
                ShopSellResponsePacket* pkt = (ShopSellResponsePacket*)data;
                pkt->message[63] = '\0';
                printf("[NET] Shop sell: %s — %s\n",
                       pkt->success ? "OK" : "FAIL", pkt->message);
                if (pkt->success) {
                    g_current_game->player.info.gold = ntohl(pkt->new_gold);
                    uint8_t slot = pkt->inventory_slot;
                    if (slot < INVENTORY_SIZE && g_current_game->inventory) {
                        g_current_game->inventory->slots[slot].template_id = 0;
                        g_current_game->inventory->slots[slot].quantity     = 0;
                    }
                }
            }
            break;

        // ----------------------------------------------------------------
        // QUEST packets
        // ----------------------------------------------------------------

        case PACKET_QUEST_ACCEPT:
            if (length >= (int)sizeof(QuestAcceptPacket) && g_current_game) {
                QuestAcceptPacket* pkt = (QuestAcceptPacket*)data;
                uint32_t quest_id = ntohl(pkt->quest_id);
                pkt->title[47] = '\0';
                uint8_t obj_count = pkt->obj_count;
                if (obj_count > MAX_QUEST_OBJECTIVES) obj_count = MAX_QUEST_OBJECTIVES;

                char descs[MAX_QUEST_OBJECTIVES][64];
                int32_t reqs[MAX_QUEST_OBJECTIVES];
                for (int i = 0; i < obj_count; i++) {
                    memcpy(descs[i], pkt->objectives[i].description, 63);
                    descs[i][63] = '\0';
                    reqs[i] = (int32_t)ntohl((uint32_t)pkt->objectives[i].required);
                }

                quest_log_add(&g_current_game->quest_log,
                              quest_id, pkt->title,
                              obj_count, descs, reqs);
                printf("[NET] Quest accepted: id=%u '%s'\n", quest_id, pkt->title);
            }
            break;

        case PACKET_QUEST_PROGRESS:
            if (length >= (int)sizeof(QuestProgressPacket) && g_current_game) {
                QuestProgressPacket* pkt = (QuestProgressPacket*)data;
                uint32_t quest_id = ntohl(pkt->quest_id);
                int32_t current  = (int32_t)ntohl((uint32_t)pkt->current);
                int32_t required = (int32_t)ntohl((uint32_t)pkt->required);
                quest_log_update_progress(&g_current_game->quest_log,
                                          quest_id, pkt->obj_index,
                                          current, required);
                printf("[NET] Quest progress: id=%u obj=%u %d/%d\n",
                       quest_id, pkt->obj_index, current, required);
            }
            break;

        case PACKET_QUEST_COMPLETE:
            if (length >= (int)sizeof(QuestCompletePacket) && g_current_game) {
                QuestCompletePacket* pkt = (QuestCompletePacket*)data;
                uint32_t quest_id  = ntohl(pkt->quest_id);
                uint32_t xp_reward = ntohl(pkt->xp_reward);
                uint32_t gold_reward = ntohl(pkt->gold_reward);

                quest_log_complete(&g_current_game->quest_log, quest_id);

                // Show reward notification (reuse existing kill-reward popup)
                for (int i = 0; i < MAX_REWARD_POPUPS; i++) {
                    if (!g_current_game->reward_notifications[i].active) {
                        g_current_game->reward_notifications[i].xp_gained   = xp_reward;
                        g_current_game->reward_notifications[i].gold_gained = gold_reward;
                        g_current_game->reward_notifications[i].age         = 0.0f;
                        g_current_game->reward_notifications[i].active      = 1;
                        break;
                    }
                }

                // Refresh gold/XP from server
                network_request_player_data_refresh();

                printf("[NET] Quest complete: id=%u +%u XP +%u gold\n",
                       quest_id, xp_reward, gold_reward);
            }
            break;

        default:
            printf("[NET] ⚠️ Unknown packet type: %d (0x%02X)\n", header->type, header->type);
            break;
    }
}

// ============================================================================
// PUBLIC API - INITIALIZATION
// ============================================================================

int network_init(uint32_t account_id) {
    if (g_initialized) return 1;
    
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        printf("[NET] WSAStartup failed\n");
        return 0;
    }
    
    g_account_id = account_id;
    g_initialized = TRUE;
    g_last_ping_time = get_time();
    
    printf("[NET] Initialized (account %u)\n", account_id);
    return 1;
}

void network_cleanup(void) {
    if (g_socket != INVALID_SOCKET) {
        closesocket(g_socket);
        g_socket = INVALID_SOCKET;
    }
    
    if (g_initialized) {
        WSACleanup();
        g_initialized = FALSE;
    }
    
    g_connected = FALSE;
    g_recv_len = 0;
}

void network_disconnect(void) {
    if (g_socket != INVALID_SOCKET) {
        if (g_connected) {
            // Send a clean logout before closing so the server can save immediately
            PacketHeader pkt;
            memset(&pkt, 0, sizeof(pkt));
            pkt.type = PACKET_LOGOUT;
            pkt.player_id = htonl(g_account_id);
            pkt.payload_size = 0;
            send(g_socket, (char*)&pkt, sizeof(pkt), 0);
        }
        closesocket(g_socket);
        g_socket = INVALID_SOCKET;
    }
    g_connected = FALSE;
    g_pending_pings = 0;
    g_recv_len = 0;
}

int network_is_connected(void) {
    return g_connected;
}

void network_set_character_id(uint32_t character_id) {
    g_character_id = character_id;
    printf("[NET] Character ID set to %u\n", character_id);
}

// ============================================================================
// PUBLIC API - UPDATE
// ============================================================================

// Returns the expected total size for a given packet type, or 0 if unknown
static int get_packet_size(uint8_t type) {
    switch (type) {
        // Basic packets
        case PACKET_PING:                       return (int)sizeof(PacketHeader);
        case PACKET_DISCONNECT:                 return (int)sizeof(PacketHeader);
        
        // Auth packets
        case PACKET_AUTH_LOGIN:                 return (int)sizeof(AuthLoginPacket);
        case PACKET_AUTH_REGISTER:              return (int)sizeof(AuthRegisterPacket);
        case PACKET_AUTH_RESPONSE:              return (int)sizeof(AuthLoginResponsePacket);
        case PACKET_START_GAME_REQUEST:         return (int)sizeof(StartGameRequestPacket);
        case PACKET_START_GAME_RESPONSE:        return (int)sizeof(StartGameResponsePacket);
        
        // Patch notes
        case PATCH_NOTES_REQUEST:               return (int)sizeof(PatchNotesRequest);
        case PATCH_NOTES_RESPONSE:              return (int)sizeof(PatchNotesResponse);
        
        // Realm server packets
        case PACKET_REALM_CONNECT:              return (int)sizeof(RealmConnectPacket);
        case PACKET_REALM_CONNECT_ACK:          return (int)sizeof(RealmConnectAckPacket);
        case PACKET_WORLD_LIST_REQUEST:         return (int)sizeof(WorldListRequestPacket);
        // PACKET_WORLD_LIST_RESPONSE - variable length, handled in network_update()
        case PACKET_CHARACTER_LIST_REQUEST:     return (int)sizeof(CharacterListRequestPacket);
        // PACKET_CHARACTER_LIST_RESPONSE - variable length, handled in network_update()
        case PACKET_CHARACTER_CREATE_REQUEST:   return (int)sizeof(CharacterCreateRequestPacket);
        case PACKET_CHARACTER_CREATE_RESPONSE:  return (int)sizeof(CharacterCreateResponsePacket);
        case PACKET_CHARACTER_DELETE_REQUEST:   return (int)sizeof(CharacterDeleteRequestPacket);
        case PACKET_CHARACTER_DELETE_RESPONSE:  return (int)sizeof(CharacterDeleteResponsePacket);
        case PACKET_ENTER_WORLD:                return (int)sizeof(EnterWorldPacket);
        case PACKET_ENTER_WORLD_RESPONSE:       return (int)sizeof(EnterWorldResponsePacket);
        
        // World server packets
        case PACKET_WORLD_CONNECT:              return (int)sizeof(WorldConnectPacket);
        case PACKET_WORLD_CONNECT_ACK:          return (int)sizeof(WorldConnectAckPacket);
        case PACKET_PLAYER_MOVE:                return (int)sizeof(PlayerMovePacket);
        case PACKET_PLAYER_MOVE_ACK:            return (int)sizeof(PlayerMoveAckPacket);
        case PACKET_REQUEST_PLAYER_DATA:        return (int)sizeof(WorldPlayerDataRequest);
        case PACKET_PLAYER_DATA_RESPONSE:       return (int)sizeof(CharacterInfo);

        // Combat packets
        case PACKET_CAST_CANCEL:                return (int)sizeof(CastCancelPacket);
        case PACKET_ATTACK_INTENT:              return (int)sizeof(AttackIntentPacket);
        case PACKET_CAST_START_V2:              return (int)sizeof(CastStartV2Packet);
        case PACKET_DAMAGE_V2:                  return (int)sizeof(DamageV2Packet);
        case PACKET_ATTACK_RESULT:              return (int)sizeof(AttackResultPacket);

        // Ability packets
        case PACKET_ABILITY_CAST_INTENT:        return (int)sizeof(AbilityCastIntentPacket);
        case PACKET_ABILITY_CAST_START:         return (int)sizeof(AbilityCastStartPacket);
        case PACKET_ABILITY_EFFECT:             return (int)sizeof(AbilityEffectPacket);
        case PACKET_ABILITY_CAST_CANCEL:        return (int)sizeof(AbilityCastCancelPacket);
        case PACKET_STATUS_EFFECT_APPLY:        return (int)sizeof(StatusEffectApplyPacket);
        case PACKET_STATUS_EFFECT_REMOVE:       return (int)sizeof(StatusEffectRemovePacket);
        case PACKET_SPAWN_ZONE:                 return (int)sizeof(SpawnZonePacket);
        case PACKET_REMOVE_ZONE:                return (int)sizeof(RemoveZonePacket);
        case PACKET_MANA_UPDATE:                return (int)sizeof(ManaUpdatePacket);
        case PACKET_ABILITY_DATA:               return (int)sizeof(AbilityDataPacket);

        // Level & Stats
        case PACKET_LEVEL_UP:                   return (int)sizeof(LevelUpPacket);
        case PACKET_PLAYER_STATS:               return (int)sizeof(PlayerStatsPacket);
        case PACKET_REQUEST_PLAYER_STATS:       return (int)sizeof(RequestPlayerStatsPacket);

        // Rewards
        case PACKET_KILL_REWARD:                return (int)sizeof(KillRewardPacket);

        // Position broadcasts - variable length, handled in network_update()
        // case PACKET_PLAYER_POSITIONS:
        // case PACKET_NPC_POSITIONS:

        // Equipment packets
        case PACKET_EQUIP_ITEM:                 return (int)sizeof(EquipItemPacket);
        case PACKET_EQUIP_ITEM_RESPONSE:        return (int)sizeof(EquipItemResponsePacket);
        case PACKET_UNEQUIP_ITEM:               return (int)sizeof(UnequipItemPacket);
        case PACKET_UNEQUIP_ITEM_RESPONSE:      return (int)sizeof(UnequipItemResponsePacket);

        // Inventory packets
        case PACKET_MOVE_ITEM:                  return (int)sizeof(MoveItemPacket);
        case PACKET_MOVE_ITEM_RESPONSE:         return (int)sizeof(MoveItemResponsePacket);
        case PACKET_USE_ITEM:                   return (int)sizeof(UseItemPacket);
        case PACKET_USE_ITEM_RESPONSE:          return (int)sizeof(UseItemResponsePacket);
        case PACKET_DROP_ITEM:                  return (int)sizeof(DropItemPacket);
        case PACKET_DROP_ITEM_RESPONSE:         return (int)sizeof(DropItemResponsePacket);

        // Dialogue packets
        case PACKET_NPC_INTERACT_REQUEST:       return (int)sizeof(NPCInteractRequestPacket);
        case PACKET_NPC_INTERACT_RESPONSE:      return (int)sizeof(NPCInteractResponsePacket);
        case PACKET_DIALOGUE_OPTION_SELECT:     return (int)sizeof(DialogueOptionSelectPacket);
        case PACKET_DIALOGUE_UPDATE:            return (int)sizeof(DialogueUpdatePacket);
        case PACKET_DIALOGUE_CLOSE:             return (int)sizeof(DialogueClosePacket);

        // Projectile packets
        case PACKET_PROJECTILE_SPAWN:           return (int)sizeof(ProjectileSpawnPacket);
        // PACKET_PROJECTILE_UPDATE - variable length, handled in network_update()
        case PACKET_PROJECTILE_DESTROY:         return (int)sizeof(ProjectileDestroyPacket);

        // Death/Respawn packets
        case PACKET_PLAYER_DEATH:               return (int)sizeof(PlayerDeathPacket);
        case PACKET_PLAYER_RESPAWN:             return (int)sizeof(PlayerRespawnPacket);

        // Loot packets
        case PACKET_LOOT_DROP:                  return (int)sizeof(LootDropPacket);
        case PACKET_LOOT_PICKUP_REQUEST:        return (int)sizeof(LootPickupRequestPacket);
        case PACKET_LOOT_PICKUP_RESPONSE:       return (int)sizeof(LootPickupResponsePacket);
        case PACKET_LOOT_DESPAWN:               return (int)sizeof(LootDespawnPacket);

        // NPC Telegraph packets
        case PACKET_NPC_TELEGRAPH_START:        return (int)sizeof(NPCTelegraphStartPacket);
        case PACKET_NPC_TELEGRAPH_RESOLVE:      return (int)sizeof(NPCTelegraphResolvePacket);

        // Chat packets
        case PACKET_CHAT_SEND:                  return (int)sizeof(ChatSendPacket);
        case PACKET_CHAT_MESSAGE:               return (int)sizeof(ChatMessagePacket);

        // Party packets
        case PACKET_PARTY_INVITE:               return (int)sizeof(PartyInvitePacket);
        case PACKET_PARTY_INVITE_NOTIFY:        return (int)sizeof(PartyInviteNotifyPacket);
        case PACKET_PARTY_ACCEPT:               return (int)sizeof(PartyAcceptPacket);
        case PACKET_PARTY_DECLINE:              return (int)sizeof(PartyDeclinePacket);
        case PACKET_PARTY_LEAVE:                return (int)sizeof(PartyLeavePacket);
        case PACKET_PARTY_KICK:                 return (int)sizeof(PartyKickPacket);
        // PACKET_PARTY_UPDATE - variable length, handled in network_update()
        case PACKET_PARTY_DISBAND:              return (int)sizeof(PartyDisbandPacket);

        // Shop packets
        case PACKET_SHOP_OPEN:                  return (int)sizeof(ShopOpenPacket);
        case PACKET_SHOP_BUY:                   return (int)sizeof(ShopBuyPacket);
        case PACKET_SHOP_BUY_RESPONSE:          return (int)sizeof(ShopBuyResponsePacket);
        case PACKET_SHOP_SELL:                  return (int)sizeof(ShopSellPacket);
        case PACKET_SHOP_SELL_RESPONSE:         return (int)sizeof(ShopSellResponsePacket);

        // Quest packets
        case PACKET_QUEST_ACCEPT:               return (int)sizeof(QuestAcceptPacket);
        case PACKET_QUEST_PROGRESS:             return (int)sizeof(QuestProgressPacket);
        case PACKET_QUEST_COMPLETE:             return (int)sizeof(QuestCompletePacket);

        // Server-to-server packets (shouldn't receive these in client, but handle anyway)
        case PACKET_REALM_AUTH:                 return 0;  // Unknown structure
        case PACKET_REALM_AUTH_ACK:             return 0;  // Unknown structure
        case PACKET_WORLD_HEARTBEAT:            return 0;  // Unknown structure
        case PACKET_WORLD_STATUS:               return 0;  // Unknown structure

        default:                                return 0;  // Unknown
    }
}

void network_update(void) {
    if (g_socket == INVALID_SOCKET || !g_connected) return;
    
    // Read as much as we can into the reassembly buffer
    while (1) {
        int space = (int)sizeof(g_recv_buf) - g_recv_len;
        if (space <= 0) break;
        
        int bytes = recv(g_socket, g_recv_buf + g_recv_len, space, 0);
        
        if (bytes > 0) {
            g_recv_len += bytes;
            NET_LOG("[NET] Received %d bytes, buffer now has %d bytes\n", bytes, g_recv_len);
        } else if (bytes == 0) {
            printf("[NET] Server closed connection\n");
            g_connected = FALSE;
            closesocket(g_socket);
            g_socket = INVALID_SOCKET;
            g_recv_len = 0;
            return;
        } else {
            int err = WSAGetLastError();
            if (err == WSAEWOULDBLOCK) break;
            printf("[NET] Socket error: %d\n", err);
            g_connected = FALSE;
            closesocket(g_socket);
            g_socket = INVALID_SOCKET;
            g_recv_len = 0;
            return;
        }
    }
    
    // Process all complete packets in the buffer
    int offset = 0;
    while (offset < g_recv_len) {
        int remaining = g_recv_len - offset;
        
        // Need at least a header
        if (remaining < (int)sizeof(PacketHeader)) {
            NET_LOG("[NET] Not enough bytes for header (have %d, need %zu)\n",
                   remaining, sizeof(PacketHeader));
            break;
        }
        
        uint8_t pkt_type = (uint8_t)g_recv_buf[offset];
        PacketHeader* hdr = (PacketHeader*)(g_recv_buf + offset);
        int pkt_size = (int)sizeof(PacketHeader) + (int)ntohs(hdr->payload_size);

        NET_LOG("[NET] Packet type %d (0x%02X) at offset %d — payload_size=%d, total=%d, remaining=%d\n",
               pkt_type, pkt_type, offset, (int)ntohs(hdr->payload_size), pkt_size, remaining);

        // Reject oversized packets (corrupted header guard)
        if (pkt_size > (int)sizeof(g_recv_buf)) {
            printf("[NET] ❌ Packet type %d (0x%02X) claims %d bytes — exceeds buffer, disconnecting\n",
                   pkt_type, pkt_type, pkt_size);
            g_connected = FALSE;
            closesocket(g_socket);
            g_socket = INVALID_SOCKET;
            g_recv_len = 0;
            return;
        }

        // Wait for the full packet
        if (remaining < pkt_size) {
            NET_LOG("[NET] Waiting for complete packet type %d (have %d, need %d)\n",
                   pkt_type, remaining, pkt_size);
            break;
        }

        // Process the complete packet
        NET_LOG("[NET] Processing complete packet type %d (%d bytes)\n", pkt_type, pkt_size);
        process_packet(g_recv_buf + offset, pkt_size);
        offset += pkt_size;
    }
    
    // Shift remaining data to start of buffer
    if (offset > 0) {
        if (offset < g_recv_len) {
            int leftover = g_recv_len - offset;
            NET_LOG("[NET] Shifting %d leftover bytes to start of buffer\n", leftover);
            memmove(g_recv_buf, g_recv_buf + offset, leftover);
            g_recv_len = leftover;
        } else {
            // Processed all data
            g_recv_len = 0;
        }
    }
}

void network_update_with_ping(int game_mode) {
    network_update();

    if (!g_connected) return;

    // Send pings in all connected states
    if (game_mode == GAME_MODE_MAIN_MENU      ||
        game_mode == GAME_MODE_SERVER_LIST     ||
        game_mode == GAME_MODE_CHARACTER_SELECT ||
        game_mode == GAME_MODE_PLAYING) {

        double now = get_time();
        if (now - g_last_ping_time >= PING_INTERVAL) {
            // Too many unanswered pings — server is unreachable
            if (g_pending_pings >= PING_MAX_MISSED) {
                printf("[NET] Ping timeout: %d pings unanswered, disconnecting\n",
                       g_pending_pings);
                network_disconnect();
                return;
            }
            network_send_ping();
            g_last_ping_time = now;
        }
    }
}

// ============================================================================
// PUBLIC API - REALM CONNECTION
// ============================================================================

int network_connect_to_realm(const char* ip, uint16_t port,
                            const char* session_key, uint32_t account_id) {
    if (!g_initialized) return 0;
    
    g_recv_len = 0;  // Clear stream buffer for new connection
    g_realm_connect_ack_ready = FALSE;  // Reset ACK flag
    
    printf("[NET] Connecting to realm %s:%u...\n", ip, port);
    
    g_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (g_socket == INVALID_SOCKET) {
        printf("[NET] Failed to create socket\n");
        return 0;
    }
    
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    
    if (inet_pton(AF_INET, ip, &addr.sin_addr) <= 0) {
        printf("[NET] Invalid IP address: %s\n", ip);
        closesocket(g_socket);
        g_socket = INVALID_SOCKET;
        return 0;
    }
    
    if (connect(g_socket, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        printf("[NET] Failed to connect to realm server\n");
        closesocket(g_socket);
        g_socket = INVALID_SOCKET;
        return 0;
    }
    
    // Set non-blocking IMMEDIATELY
    u_long mode = 1;
    ioctlsocket(g_socket, FIONBIO, &mode);
    
    // Send connect packet
    RealmConnectPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.header.type = PACKET_REALM_CONNECT;
    pkt.header.player_id = htonl(account_id);
    pkt.header.payload_size = 0;
    memcpy(pkt.header.session_key, session_key, 32);
    
    if (send(g_socket, (char*)&pkt, sizeof(pkt), 0) != sizeof(pkt)) {
        printf("[NET] Failed to send realm connect packet\n");
        closesocket(g_socket);
        g_socket = INVALID_SOCKET;
        return 0;
    }
    
    // Wait for ACK using network_update()
    printf("[NET] Waiting for realm server ACK...\n");
    double start_time = get_time();
    double timeout = 10.0;
    
    g_connected = TRUE;  // Temporarily set to allow network_update()
    
    while (!g_realm_connect_ack_ready) {
        network_update();
        
        if (get_time() - start_time > timeout) {
            printf("[NET] Timeout waiting for realm server ACK\n");
            g_connected = FALSE;
            closesocket(g_socket);
            g_socket = INVALID_SOCKET;
            return 0;
        }
        
        Sleep(10);
    }
    
    if (!g_realm_connect_ack.success) {
        printf("[NET] Realm server rejected connection: %s\n", 
               g_realm_connect_ack.message);
        g_connected = FALSE;
        closesocket(g_socket);
        g_socket = INVALID_SOCKET;
        return 0;
    }
    
    g_connected = TRUE;
    g_pending_pings = 0;
    g_last_ping_time = get_time();
    printf("[NET] Connected to realm: %s\n", g_realm_connect_ack.message);
    return 1;
}

int network_request_world_list(void) {
    if (!g_connected) return 0;
    
    g_world_list_ready = FALSE;
    
    WorldListRequestPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.header.type = PACKET_WORLD_LIST_REQUEST;
    pkt.header.player_id = htonl(g_account_id);
    pkt.header.payload_size = htons(sizeof(WorldListRequestPacket) - sizeof(PacketHeader));
    
    return send(g_socket, (char*)&pkt, sizeof(pkt), 0) == sizeof(pkt);
}

int network_get_world_list(WorldListResponsePacket* out) {
    if (!g_world_list_ready) return 0;
    
    memcpy(out, &g_world_list, sizeof(WorldListResponsePacket));
    g_world_list_ready = FALSE;
    return 1;
}

int network_request_character_list(uint32_t world_id) {
    if (!g_connected) return 0;
    
    g_char_list_ready = FALSE;
    
    CharacterListRequestPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.header.type = PACKET_CHARACTER_LIST_REQUEST;
    pkt.header.player_id = htonl(g_account_id);
    pkt.header.payload_size = htons(sizeof(CharacterListRequestPacket) - sizeof(PacketHeader));
    pkt.world_id = htonl(world_id);
    
    return send(g_socket, (char*)&pkt, sizeof(pkt), 0) == sizeof(pkt);
}

int network_get_character_list(CharacterListResponsePacket* out) {
    if (!g_char_list_ready) return 0;
    
    memcpy(out, &g_char_list, sizeof(CharacterListResponsePacket));
    g_char_list_ready = FALSE;
    return 1;
}

int network_create_character(uint32_t world_id, const char* name,
                            uint32_t class_id, uint32_t race_id) {
    if (!g_connected) return 0;
    
    g_char_create_ready = FALSE;
    
    CharacterCreateRequestPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.header.type = PACKET_CHARACTER_CREATE_REQUEST;
    pkt.header.player_id = htonl(g_account_id);
    pkt.header.payload_size = htons(sizeof(CharacterCreateRequestPacket) - sizeof(PacketHeader));
    pkt.world_id = htonl(world_id);
    strncpy(pkt.name, name, 31);
    pkt.class_id = htonl(class_id);
    pkt.race_id = htonl(race_id);
    
    return send(g_socket, (char*)&pkt, sizeof(pkt), 0) == sizeof(pkt);
}

int network_get_character_create_response(CharacterCreateResponsePacket* out) {
    if (!g_char_create_ready) return 0;

    memcpy(out, &g_char_create, sizeof(CharacterCreateResponsePacket));
    g_char_create_ready = FALSE;
    return 1;
}

int network_delete_character(uint32_t world_id, uint32_t character_id) {
    if (!g_connected) return 0;

    g_char_delete_ready = FALSE;

    CharacterDeleteRequestPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.header.type = PACKET_CHARACTER_DELETE_REQUEST;
    pkt.header.player_id = htonl(g_account_id);
    pkt.header.payload_size = htons(sizeof(CharacterDeleteRequestPacket) - sizeof(PacketHeader));
    pkt.character_id = htonl(character_id);
    pkt.world_id = htonl(world_id);

    return send(g_socket, (char*)&pkt, sizeof(pkt), 0) == sizeof(pkt);
}

int network_get_character_delete_response(CharacterDeleteResponsePacket* out) {
    if (!g_char_delete_ready) return 0;

    memcpy(out, &g_char_delete, sizeof(CharacterDeleteResponsePacket));
    g_char_delete_ready = FALSE;
    return 1;
}

int network_request_enter_world(uint32_t character_id, uint32_t world_id) {
    if (!g_connected) return 0;
    
    g_enter_world_ready = FALSE;
    
    EnterWorldPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.header.type = PACKET_ENTER_WORLD;
    pkt.header.player_id = htonl(g_account_id);
    pkt.header.payload_size = htons(sizeof(EnterWorldPacket) - sizeof(PacketHeader));
    pkt.character_id = htonl(character_id);
    pkt.world_id = htonl(world_id);
    
    printf("[NET] Requesting enter world (char %u, world %u)\n", character_id, world_id);
    return send(g_socket, (char*)&pkt, sizeof(pkt), 0) == sizeof(pkt);
}

int network_get_enter_world_response(EnterWorldResponsePacket* out) {
    if (!g_enter_world_ready) return 0;
    
    memcpy(out, &g_enter_world, sizeof(EnterWorldResponsePacket));
    g_enter_world_ready = FALSE;
    return 1;
}

// ============================================================================
// PUBLIC API - WORLD CONNECTION
// ============================================================================

int network_connect_to_world(const char* ip, uint16_t port,
                            const char* game_ticket, uint32_t character_id) {
    if (!g_initialized) return 0;
    
    g_recv_len = 0;  // Clear stream buffer for new connection
    g_world_connect_ack_ready = FALSE;  // Reset ACK flag
    
    printf("[NET] Connecting to world %s:%u...\n", ip, port);
    
    // Close realm connection first
    if (g_socket != INVALID_SOCKET) {
        closesocket(g_socket);
    }
    
    g_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (g_socket == INVALID_SOCKET) {
        printf("[NET] Failed to create socket\n");
        return 0;
    }
    
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    
    if (inet_pton(AF_INET, ip, &addr.sin_addr) <= 0) {
        printf("[NET] Invalid IP address: %s\n", ip);
        closesocket(g_socket);
        g_socket = INVALID_SOCKET;
        return 0;
    }
    
    if (connect(g_socket, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        printf("[NET] Failed to connect to world server\n");
        closesocket(g_socket);
        g_socket = INVALID_SOCKET;
        return 0;
    }
    
    // Set non-blocking IMMEDIATELY (before any recv)
    u_long mode = 1;
    ioctlsocket(g_socket, FIONBIO, &mode);
    
    // Send world connect packet
    WorldConnectPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.header.type = PACKET_WORLD_CONNECT;
    pkt.header.player_id = htonl(character_id);
    pkt.header.payload_size = htons(sizeof(WorldConnectPacket) - sizeof(PacketHeader));
    memcpy(pkt.game_ticket, game_ticket, 64);
    pkt.character_id = htonl(character_id);
    
    if (send(g_socket, (char*)&pkt, sizeof(pkt), 0) != sizeof(pkt)) {
        printf("[NET] Failed to send world connect packet\n");
        closesocket(g_socket);
        g_socket = INVALID_SOCKET;
        return 0;
    }
    
    // Wait for ACK by calling network_update() in a loop
    printf("[NET] Waiting for world server ACK...\n");
    double start_time = get_time();
    double timeout = 5.0;  // 5 second timeout
    
    g_connected = TRUE;  // Temporarily set to allow network_update() to work
    
    while (!g_world_connect_ack_ready) {
        network_update();  // This will process incoming packets including the ACK
        
        if (get_time() - start_time > timeout) {
            printf("[NET] Timeout waiting for world server ACK\n");
            g_connected = FALSE;
            closesocket(g_socket);
            g_socket = INVALID_SOCKET;
            return 0;
        }
        
        // Small sleep to avoid spinning CPU
        Sleep(10);
    }
    
    // Check if ACK was successful
    if (!g_world_connect_ack.success) {
        printf("[NET] World server rejected connection: %s\n", 
               g_world_connect_ack.welcome_message);
        g_connected = FALSE;
        closesocket(g_socket);
        g_socket = INVALID_SOCKET;
        return 0;
    }
    
    g_connected = TRUE;
    g_pending_pings = 0;
    g_last_ping_time = get_time();
    printf("[NET] Connected to world server: %s\n", g_world_connect_ack.welcome_message);
    return 1;
}

int network_request_character_data(uint32_t character_id, uint32_t world_id) {
    if (!g_connected) return 0;
    
    g_char_data_ready = FALSE;
    
    WorldPlayerDataRequest pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.header.type = PACKET_REQUEST_PLAYER_DATA;
    pkt.header.player_id = htonl(g_account_id);
    pkt.header.payload_size = htons(sizeof(WorldPlayerDataRequest) - sizeof(PacketHeader));
    pkt.character_id = htonl(character_id);
    pkt.world_id = htonl(world_id);

    return send(g_socket, (char*)&pkt, sizeof(pkt), 0) == sizeof(pkt);
}

int network_get_character_data(CharacterInfo* out) {
    if (!g_char_data_ready) return 0;
    
    memcpy(out, &g_char_data, sizeof(CharacterInfo));
    
    // Convert from network byte order
    out->level = ntohl(out->level);
    out->health = ntohl(out->health);
    out->max_health = ntohl(out->max_health);
    out->mana = (int32_t)ntohl((uint32_t)out->mana);
    out->max_mana = (int32_t)ntohl((uint32_t)out->max_mana);
    out->experience = ntohll(out->experience);

    out->player_class = ntohl(out->player_class);
    out->player_race  = ntohl(out->player_race);

    out->gold = ntohl(out->gold);
    out->helmet = ntohl(out->helmet);
    out->gloves = ntohl(out->gloves);
    out->chest_armor = ntohl(out->chest_armor);
    out->leggings = ntohl(out->leggings);
    out->boots = ntohl(out->boots);
    out->main_hand = ntohl(out->main_hand);
    out->second_hand = ntohl(out->second_hand);
    out->blessing = ntohs(out->blessing);
    
    for (int i = 0; i < 150; i++) {
        out->inventory[i] = ntohl(out->inventory[i]);
    }
    
    g_char_data_ready = FALSE;
    return 1;
}

int network_send_player_move(float x, float y, float speed, float vel_x, float vel_y) {
    if (!g_connected) return 0;
    
    PlayerMovePacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.header.type = PACKET_PLAYER_MOVE;
    pkt.header.player_id = htonl(g_account_id);
    pkt.header.payload_size = htons(sizeof(PlayerMovePacket) - sizeof(PacketHeader));
    pkt.pos_x = x;
    pkt.pos_y = y;
    pkt.player_speed = speed;
    pkt.vel_x = vel_x;
    pkt.vel_y = vel_y;
    
    return send(g_socket, (char*)&pkt, sizeof(pkt), 0) == sizeof(pkt);
}

int network_get_server_correction(float* out_x, float* out_y) {
    if (!g_correction_ready) return 0;
    
    *out_x = g_correction_x;
    *out_y = g_correction_y;
    g_correction_ready = FALSE;
    return 1;
}

void network_send_ping(void) {
    if (!g_connected) return;

    // Pack header + uint16_t ping_ms payload so the server can store our RTT
    struct {
        PacketHeader header;
        uint16_t     ping_ms;
    } pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.header.type         = PACKET_PING;
    pkt.header.player_id    = htonl(g_account_id);
    pkt.header.payload_size = htons(sizeof(uint16_t));
    pkt.ping_ms             = htons((uint16_t)g_ping_ms);

    if (send(g_socket, (char*)&pkt, sizeof(pkt), 0) == sizeof(pkt)) {
        g_pending_pings++;
        g_ping_send_time = get_time();
    }
}

int network_get_ping_ms(void) {
    return g_ping_ms;
}

// ============================================================================
// PUBLIC API - COMBAT
// ============================================================================

void network_update_facing_direction(float vel_x, float vel_y) {
    if (vel_x != 0.0f || vel_y != 0.0f) {
        g_last_facing_angle = atan2f(vel_y, vel_x);
    }
}

void network_send_attack_intent(float aim_x, float aim_y) {
    if (!g_connected) return;
    
    AttackIntentPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.header.type = PACKET_ATTACK_INTENT;
    pkt.header.player_id = htonl(g_character_id);
    pkt.header.payload_size = htons(sizeof(AttackIntentPacket) - sizeof(PacketHeader));
    pkt.aim_x = aim_x;
    pkt.aim_y = aim_y;
    
    if (send(g_socket, (char*)&pkt, sizeof(pkt), 0) == sizeof(pkt)) {
        printf("[NET] Attack intent sent: aim (%.1f, %.1f)\n", aim_x, aim_y);
    }
}

void network_send_ability_cast(uint16_t ability_id, float aim_x, float aim_y, uint32_t target_id) {
    if (!g_connected) return;

    AbilityCastIntentPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.header.type = PACKET_ABILITY_CAST_INTENT;
    pkt.header.player_id = htonl(g_character_id);
    pkt.header.payload_size = htons(sizeof(AbilityCastIntentPacket) - sizeof(PacketHeader));
    pkt.ability_id = htons(ability_id);
    pkt.aim_x = aim_x;
    pkt.aim_y = aim_y;
    pkt.target_id = htonl(target_id);

    if (send(g_socket, (char*)&pkt, sizeof(pkt), 0) == sizeof(pkt)) {
        printf("[NET] Ability cast intent sent: id=%u aim=(%.1f, %.1f)\n",
               ability_id, aim_x, aim_y);
    }
}

void network_send_ability_cancel(void) {
    if (!g_connected) return;

    AbilityCastCancelPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.header.type = PACKET_ABILITY_CAST_CANCEL;
    pkt.header.player_id = htonl(g_character_id);
    pkt.header.payload_size = htons(sizeof(AbilityCastCancelPacket) - sizeof(PacketHeader));
    pkt.caster_id = htonl(g_character_id);
    pkt.ability_id = 0;
    pkt.reason = 0;

    send(g_socket, (char*)&pkt, sizeof(pkt), 0);
}

// ============================================================================
// PUBLIC API - STATS
// ============================================================================

void network_request_player_stats(void) {
    if (!g_connected) return;

    RequestPlayerStatsPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.header.type = PACKET_REQUEST_PLAYER_STATS;
    pkt.header.player_id = htonl(g_character_id);
    pkt.header.payload_size = 0; // Header-only packet

    send(g_socket, (char*)&pkt, sizeof(pkt), 0);
    printf("[NET] Requested player stats refresh\n");
}

void network_request_player_data_refresh(void) {
    if (!g_connected || g_character_id == 0) return;

    WorldPlayerDataRequest pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.header.type = PACKET_REQUEST_PLAYER_DATA;
    pkt.header.player_id = htonl(g_account_id);
    pkt.header.payload_size = htons(sizeof(WorldPlayerDataRequest) - sizeof(PacketHeader));
    pkt.character_id = htonl(g_character_id);
    pkt.world_id = 0;

    send(g_socket, (char*)&pkt, sizeof(pkt), 0);
    printf("[NET] Requested player data refresh (XP/gold)\n");
}

// ============================================================================
// PUBLIC API - NPC DIALOGUE
// ============================================================================

void network_send_npc_interact_request(uint32_t npc_id) {
    if (!g_connected) return;

    NPCInteractRequestPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.header.type = PACKET_NPC_INTERACT_REQUEST;
    pkt.header.player_id = htonl(g_character_id);
    pkt.header.payload_size = htons(sizeof(NPCInteractRequestPacket) - sizeof(PacketHeader));
    pkt.npc_id = htonl(npc_id);

    if (send(g_socket, (char*)&pkt, sizeof(pkt), 0) == sizeof(pkt)) {
        printf("[NET] NPC interact request sent for NPC %u\n", npc_id);
    }
}

void network_send_dialogue_option_select(uint32_t npc_id, uint32_t dialogue_id, uint8_t current_page, uint8_t option_selected) {
    if (!g_connected) return;

    DialogueOptionSelectPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.header.type = PACKET_DIALOGUE_OPTION_SELECT;
    pkt.header.player_id = htonl(g_character_id);
    pkt.header.payload_size = htons(sizeof(DialogueOptionSelectPacket) - sizeof(PacketHeader));
    pkt.npc_id = htonl(npc_id);
    pkt.dialogue_id = htonl(dialogue_id);
    pkt.current_page = current_page;
    pkt.option_selected = option_selected;

    if (send(g_socket, (char*)&pkt, sizeof(pkt), 0) == sizeof(pkt)) {
        printf("[NET] Dialogue option %u selected (page %u, dialogue %u)\n",
               option_selected, current_page, dialogue_id);
    }
}

int network_get_npc_interact_response(NPCInteractResponsePacket* out) {
    if (!g_npc_interact_response_ready) {
        return 0;
    }

    memcpy(out, &g_npc_interact_response, sizeof(NPCInteractResponsePacket));
    g_npc_interact_response_ready = FALSE;
    return 1;
}

int network_get_dialogue_update(DialogueUpdatePacket* out) {
    if (!g_dialogue_update_ready) {
        return 0;
    }

    memcpy(out, &g_dialogue_update, sizeof(DialogueUpdatePacket));
    g_dialogue_update_ready = FALSE;
    return 1;
}

int network_get_dialogue_close(DialogueClosePacket* out) {
    if (!g_dialogue_close_ready) {
        return 0;
    }

    memcpy(out, &g_dialogue_close, sizeof(DialogueClosePacket));
    g_dialogue_close_ready = FALSE;
    return 1;
}

// ============================================================================
// PUBLIC API - INVENTORY / EQUIPMENT
// ============================================================================

void network_send_equip_item(uint32_t item_id, uint8_t inventory_slot, uint8_t equip_slot) {
    if (!g_connected) return;

    EquipItemPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.header.type = PACKET_EQUIP_ITEM;
    pkt.header.player_id = htonl(g_character_id);
    pkt.header.payload_size = htons(sizeof(EquipItemPacket) - sizeof(PacketHeader));
    pkt.item_id = htonl(item_id);
    pkt.inventory_slot = inventory_slot;
    pkt.equip_slot = equip_slot;

    send(g_socket, (char*)&pkt, sizeof(pkt), 0);
    printf("[NET] Equip item sent: item=%u inv_slot=%u equip_slot=%u\n",
           item_id, inventory_slot, equip_slot);
}

void network_send_unequip_item(uint8_t equip_slot) {
    if (!g_connected) return;

    UnequipItemPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.header.type = PACKET_UNEQUIP_ITEM;
    pkt.header.player_id = htonl(g_character_id);
    pkt.header.payload_size = htons(sizeof(UnequipItemPacket) - sizeof(PacketHeader));
    pkt.equip_slot = equip_slot;

    send(g_socket, (char*)&pkt, sizeof(pkt), 0);
    printf("[NET] Unequip item sent: equip_slot=%u\n", equip_slot);
}

void network_send_use_item(uint8_t inventory_slot) {
    if (!g_connected) return;

    UseItemPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.header.type = PACKET_USE_ITEM;
    pkt.header.player_id = htonl(g_character_id);
    pkt.header.payload_size = htons(sizeof(UseItemPacket) - sizeof(PacketHeader));
    pkt.inventory_slot = inventory_slot;

    send(g_socket, (char*)&pkt, sizeof(pkt), 0);
    printf("[NET] Use item sent: slot=%u\n", inventory_slot);
}

void network_send_drop_item(uint8_t inventory_slot) {
    if (!g_connected) return;

    DropItemPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.header.type = PACKET_DROP_ITEM;
    pkt.header.player_id = htonl(g_character_id);
    pkt.header.payload_size = htons(sizeof(DropItemPacket) - sizeof(PacketHeader));
    pkt.inventory_slot = inventory_slot;

    send(g_socket, (char*)&pkt, sizeof(pkt), 0);
    printf("[NET] Drop item sent: slot=%u\n", inventory_slot);
}

void network_send_move_item(uint8_t from_slot, uint8_t to_slot) {
    if (!g_connected) return;

    MoveItemPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.header.type = PACKET_MOVE_ITEM;
    pkt.header.player_id = htonl(g_character_id);
    pkt.header.payload_size = htons(sizeof(MoveItemPacket) - sizeof(PacketHeader));
    pkt.from_slot = from_slot;
    pkt.to_slot = to_slot;

    send(g_socket, (char*)&pkt, sizeof(pkt), 0);
}

// ============================================================================
// PUBLIC API - CHAT
// ============================================================================

void network_send_chat(uint8_t channel, const char* message) {
    if (!g_connected) return;

    ChatSendPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.header.type = PACKET_CHAT_SEND;
    pkt.header.player_id = htonl(g_character_id);
    pkt.header.payload_size = htons(sizeof(ChatSendPacket) - sizeof(PacketHeader));
    pkt.channel = channel;
    strncpy(pkt.message, message, MAX_CHAT_MESSAGE - 1);

    send(g_socket, (char*)&pkt, sizeof(pkt), 0);
}

// ============================================================================
// PUBLIC API - LOOT
// ============================================================================

void network_send_loot_pickup(uint32_t ground_item_id) {
    if (!g_connected) return;

    LootPickupRequestPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.header.type = PACKET_LOOT_PICKUP_REQUEST;
    pkt.header.player_id = htonl(g_character_id);
    pkt.header.payload_size = htons(sizeof(LootPickupRequestPacket) - sizeof(PacketHeader));
    pkt.ground_item_id = htonl(ground_item_id);

    send(g_socket, (char*)&pkt, sizeof(pkt), 0);
    printf("[NET] Loot pickup request sent: ground_item=%u\n", ground_item_id);
}

// ============================================================================
// PUBLIC API - PARTY
// ============================================================================

void network_send_party_invite(const char* target_name) {
    if (!g_connected) return;

    PartyInvitePacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.header.type = PACKET_PARTY_INVITE;
    pkt.header.player_id = htonl(g_character_id);
    pkt.header.payload_size = htons(sizeof(PartyInvitePacket) - sizeof(PacketHeader));
    strncpy(pkt.target_name, target_name, 31);

    send(g_socket, (char*)&pkt, sizeof(pkt), 0);
    printf("[NET] Party invite sent to: %s\n", target_name);
}

void network_send_party_accept(void) {
    if (!g_connected) return;

    PartyAcceptPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.header.type = PACKET_PARTY_ACCEPT;
    pkt.header.player_id = htonl(g_character_id);

    send(g_socket, (char*)&pkt, sizeof(pkt), 0);
    printf("[NET] Party accept sent\n");
}

void network_send_party_decline(void) {
    if (!g_connected) return;

    PartyDeclinePacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.header.type = PACKET_PARTY_DECLINE;
    pkt.header.player_id = htonl(g_character_id);

    send(g_socket, (char*)&pkt, sizeof(pkt), 0);
    printf("[NET] Party decline sent\n");
}

void network_send_party_leave(void) {
    if (!g_connected) return;

    PartyLeavePacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.header.type = PACKET_PARTY_LEAVE;
    pkt.header.player_id = htonl(g_character_id);

    send(g_socket, (char*)&pkt, sizeof(pkt), 0);
    printf("[NET] Party leave sent\n");
}

void network_send_party_kick(uint32_t target_id) {
    if (!g_connected) return;

    PartyKickPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.header.type = PACKET_PARTY_KICK;
    pkt.header.player_id = htonl(g_character_id);
    pkt.header.payload_size = htons(sizeof(PartyKickPacket) - sizeof(PacketHeader));
    pkt.target_id = htonl(target_id);

    send(g_socket, (char*)&pkt, sizeof(pkt), 0);
    printf("[NET] Party kick sent: %u\n", target_id);
}

// ============================================================================
// PUBLIC API - SHOP
// ============================================================================

void network_send_shop_buy(uint32_t shop_id, uint32_t item_id) {
    if (!g_connected) return;

    ShopBuyPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.header.type = PACKET_SHOP_BUY;
    pkt.header.player_id = htonl(g_character_id);
    pkt.header.payload_size = htons(sizeof(ShopBuyPacket) - sizeof(PacketHeader));
    pkt.shop_id = htonl(shop_id);
    pkt.item_id = htonl(item_id);

    send(g_socket, (char*)&pkt, sizeof(pkt), 0);
    printf("[NET] Shop buy: shop=%u item=%u\n", shop_id, item_id);
}

void network_send_shop_sell(uint32_t shop_id, uint8_t inventory_slot) {
    if (!g_connected) return;

    ShopSellPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.header.type = PACKET_SHOP_SELL;
    pkt.header.player_id = htonl(g_character_id);
    pkt.header.payload_size = htons(sizeof(ShopSellPacket) - sizeof(PacketHeader));
    pkt.shop_id = htonl(shop_id);
    pkt.inventory_slot = inventory_slot;

    send(g_socket, (char*)&pkt, sizeof(pkt), 0);
    printf("[NET] Shop sell: shop=%u slot=%u\n", shop_id, inventory_slot);
}