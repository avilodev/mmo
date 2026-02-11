#include "ability_bar.h"
#include "network.h"
#include "game_types.h"
#include "combat_system.h"
#include "combat_render.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <math.h>

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

static EnterWorldResponsePacket g_enter_world;
static BOOL g_enter_world_ready = FALSE;

static CharacterInfo g_char_data;
static BOOL g_char_data_ready = FALSE;

static float g_correction_x = 0.0f;
static float g_correction_y = 0.0f;
static BOOL g_correction_ready = FALSE;

static float g_last_facing_angle = 0.0f;

// Ping tracking
#define PING_INTERVAL 10.0
static double g_last_ping_time = 0.0;

// Stream reassembly buffer for TCP framing
static char g_recv_buf[65536];
static int  g_recv_len = 0;

// External game state for combat events
extern GameState* g_current_game;

// ============================================================================
// HELPERS
// ============================================================================

static double get_time(void) {
    return (double)clock() / CLOCKS_PER_SEC;
}

// ============================================================================
// PACKET PROCESSOR
// ============================================================================

static void process_packet(const char* data, int length) {
    if (length < (int)sizeof(PacketHeader)) return;
    
    PacketHeader* header = (PacketHeader*)data;
    
    switch (header->type) {
        case PACKET_PING:
            break;
            
        case PACKET_WORLD_LIST_RESPONSE:
            if (length >= (int)sizeof(WorldListResponsePacket)) {
                memcpy(&g_world_list, data, sizeof(WorldListResponsePacket));
                g_world_list_ready = TRUE;
                printf("[NET] World list: %d worlds\n", g_world_list.count);
            }
            break;
            
        case PACKET_CHARACTER_LIST_RESPONSE:
            if (length >= (int)sizeof(CharacterListResponsePacket)) {
                memcpy(&g_char_list, data, sizeof(CharacterListResponsePacket));
                g_char_list_ready = TRUE;
                printf("[NET] Character list: %d chars\n", g_char_list.count);
            }
            break;
            
        case PACKET_CHARACTER_CREATE_RESPONSE:
            if (length >= (int)sizeof(CharacterCreateResponsePacket)) {
                memcpy(&g_char_create, data, sizeof(CharacterCreateResponsePacket));
                g_char_create_ready = TRUE;
            }
            break;
            
        case PACKET_ENTER_WORLD_RESPONSE:
            if (length >= (int)sizeof(EnterWorldResponsePacket)) {
                memcpy(&g_enter_world, data, sizeof(EnterWorldResponsePacket));
                g_enter_world_ready = TRUE;
            }
            break;
            
        case PACKET_WORLD_CONNECT_ACK:
            break;
            
        case PACKET_PLAYER_DATA_RESPONSE:
            if (length >= (int)sizeof(CharacterInfo)) {
                memcpy(&g_char_data, data, sizeof(CharacterInfo));
                g_char_data_ready = TRUE;
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

                if (g_current_game) {
                    float tx = 0, ty = 0;
                    for (int i = 0; i < g_current_game->visible_npc_count; i++) {
                        if (g_current_game->visible_npcs[i].npc_id == target) {
                            tx = g_current_game->visible_npcs[i].pos_x;
                            ty = g_current_game->visible_npcs[i].pos_y;
                            break;
                        }
                    }

                    if (damage == 0 && !pkt->is_kill) {
                        // MISS — show as a special event
                        combat_on_damage(&g_current_game->combat,
                                         target, -1, 0, 0, tx, ty);
                    } else {
                        combat_on_damage(&g_current_game->combat,
                                         target, (int)damage, 0, pkt->is_kill, tx, ty);
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

                    // Resolve cast on the ability bar (starts cooldown)
                    ability_bar_on_cast_resolve(&g_current_game->ability_bar, ability_id);

                    // Show damage/healing/miss number
                    float tx = 0, ty = 0;
                    for (int i = 0; i < g_current_game->visible_npc_count; i++) {
                        if (g_current_game->visible_npcs[i].npc_id == target) {
                            tx = g_current_game->visible_npcs[i].pos_x;
                            ty = g_current_game->visible_npcs[i].pos_y;
                            break;
                        }
                    }

                    if (damage == 0 && healing == 0 && !pkt->is_kill) {
                        // MISS
                        combat_on_damage(&g_current_game->combat,
                                         target, -1, 0, 0, tx, ty);
                    } else if (damage > 0) {
                        combat_on_damage(&g_current_game->combat,
                                         target, damage, 0, pkt->is_kill, tx, ty);
                    }

                    printf("[NET] Ability effect: id=%u target=%u dmg=%d heal=%d\n",
                           ability_id, target, damage, healing);
                }
            }
            break;

        case PACKET_ABILITY_CAST_CANCEL:
            if (length >= (int)sizeof(AbilityCastCancelPacket)) {
                if (g_current_game) {
                    ability_bar_on_cast_cancel(&g_current_game->ability_bar);
                    printf("[NET] Ability cast cancelled\n");
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
                // TODO: Add to visible zones array for rendering
            }
            break;

        case PACKET_REMOVE_ZONE:
            if (length >= (int)sizeof(RemoveZonePacket)) {
                RemoveZonePacket* pkt = (RemoveZonePacket*)data;
                printf("[NET] Zone removed: id=%u\n", (unsigned int)ntohl(pkt->zone_id));
                // TODO: Remove from visible zones array
            }
            break;
            
        case PACKET_NPC_POSITIONS:
            if (length >= (int)sizeof(NPCPositionPacket)) {
                NPCPositionPacket* pkt = (NPCPositionPacket*)data;

                printf("[NET] NPC positions received: %d NPCs\n", pkt->npc_count);
                
                if (g_current_game) {
                    g_current_game->visible_npc_count = pkt->npc_count;
                    
                    for (int i = 0; i < pkt->npc_count && i < MAX_VISIBLE_NPCS; i++) {
                        NPCPositionData* src = &pkt->npcs[i];
                        g_current_game->visible_npcs[i].npc_id = ntohl(src->npc_id);
                        g_current_game->visible_npcs[i].pos_x = src->pos_x;
                        g_current_game->visible_npcs[i].pos_y = src->pos_y;
                        g_current_game->visible_npcs[i].health = ntohl(src->health);
                        g_current_game->visible_npcs[i].max_health = ntohl(src->max_health);
                        g_current_game->visible_npcs[i].is_alive = src->is_alive;
                        snprintf(g_current_game->visible_npcs[i].name, 32, "NPC_%u",
                                g_current_game->visible_npcs[i].npc_id);
                    }
                }
            }
            break;

        case PACKET_LEVEL_UP:
            if (length >= (int)sizeof(LevelUpPacket)) {
                LevelUpPacket* pkt = (LevelUpPacket*)data;
                if (g_current_game) {
                    uint32_t new_level    = ntohl(pkt->new_level);
                    uint32_t new_hp       = ntohl(pkt->new_health);
                    uint32_t new_max_hp   = ntohl(pkt->new_max_health);
                    uint32_t new_mana     = ntohl(pkt->new_mana);
                    uint32_t new_max_mana = ntohl(pkt->new_max_mana);

                    // Update player info
                    g_current_game->player.info.level      = new_level;
                    g_current_game->player.info.health     = new_hp;
                    g_current_game->player.info.max_health = new_max_hp;

                    // Update stats
                    g_current_game->player_strength      = (int32_t)ntohl(pkt->strength);
                    g_current_game->player_agility       = (int32_t)ntohl(pkt->agility);
                    g_current_game->player_intelligence   = (int32_t)ntohl(pkt->intelligence);
                    g_current_game->player_wisdom         = (int32_t)ntohl(pkt->wisdom);
                    g_current_game->player_defense        = (int32_t)ntohl(pkt->defense);
                    g_current_game->player_evasion        = (int32_t)ntohl(pkt->evasion);
                    g_current_game->player_xp_for_next    = ntohll(pkt->xp_for_next_level);

                    // Update mana bar
                    ability_bar_on_mana_update(&g_current_game->ability_bar,
                                               (int32_t)new_mana, (int32_t)new_max_mana);

                    // Trigger level-up notification
                    g_current_game->show_level_up      = 1;
                    g_current_game->level_up_timer      = 3.0f;
                    g_current_game->level_up_new_level  = (int)new_level;

                    printf("[NET] LEVEL UP! Now level %u (HP=%u/%u, Mana=%u/%u)\n",
                           new_level, new_hp, new_max_hp, new_mana, new_max_mana);
                    printf("[NET] Stats: STR=%d AGI=%d INT=%d WIS=%d DEF=%d EVA=%d\n",
                           g_current_game->player_strength,
                           g_current_game->player_agility,
                           g_current_game->player_intelligence,
                           g_current_game->player_wisdom,
                           g_current_game->player_defense,
                           g_current_game->player_evasion);
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
                    g_current_game->player_move_speed     = pkt->move_speed;
                    g_current_game->player_xp_for_next    = ntohll(pkt->xp_for_next_level);

                    // Update HP and mana from stats packet (includes current values)
                    int32_t max_hp       = (int32_t)ntohl(pkt->max_health);
                    int32_t max_mana     = (int32_t)ntohl(pkt->max_mana);
                    int32_t current_hp   = (int32_t)ntohl(pkt->current_health);
                    int32_t current_mana = (int32_t)ntohl(pkt->current_mana);

                    g_current_game->player.info.max_health = (uint32_t)max_hp;
                    if (current_hp > 0) {
                        g_current_game->player.info.health = (uint32_t)current_hp;
                    }

                    // Set both current and max mana on the ability bar
                    ability_bar_on_mana_update(&g_current_game->ability_bar,
                                               current_mana, max_mana);

                    // Use server speed for player movement
                    if (g_current_game->player_move_speed > 0.0f) {
                        g_current_game->player.speed = g_current_game->player_move_speed;
                    }

                    printf("[NET] Stats received: STR=%d AGI=%d INT=%d WIS=%d DEF=%d EVA=%d speed=%.0f HP=%d/%d Mana=%d/%d\n",
                           g_current_game->player_strength,
                           g_current_game->player_agility,
                           g_current_game->player_intelligence,
                           g_current_game->player_wisdom,
                           g_current_game->player_defense,
                           g_current_game->player_evasion,
                           g_current_game->player_move_speed,
                           current_hp, max_hp,
                           current_mana, max_mana);
                }
            }
            break;
            
        case PACKET_DISCONNECT:
            g_connected = FALSE;
            break;
            
        default:
            printf("[NET] Unknown packet: %d\n", header->type);
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
        closesocket(g_socket);
        g_socket = INVALID_SOCKET;
    }
    g_connected = FALSE;
    g_recv_len = 0;  // Clear stream buffer on disconnect
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
        case PACKET_PING:                   return (int)sizeof(PacketHeader);
        case PACKET_DISCONNECT:             return (int)sizeof(PacketHeader);
        case PACKET_WORLD_LIST_RESPONSE:    return (int)sizeof(WorldListResponsePacket);
        case PACKET_CHARACTER_LIST_RESPONSE: return (int)sizeof(CharacterListResponsePacket);
        case PACKET_CHARACTER_CREATE_RESPONSE: return (int)sizeof(CharacterCreateResponsePacket);
        case PACKET_ENTER_WORLD_RESPONSE:   return (int)sizeof(EnterWorldResponsePacket);
        case PACKET_WORLD_CONNECT_ACK:      return (int)sizeof(WorldConnectAckPacket);
        case PACKET_PLAYER_DATA_RESPONSE:   return (int)sizeof(CharacterInfo);
        case PACKET_PLAYER_MOVE_ACK:        return (int)sizeof(PlayerMoveAckPacket);
        case PACKET_ATTACK_RESULT:          return (int)sizeof(AttackResultPacket);
        case PACKET_CAST_START_V2:          return (int)sizeof(CastStartV2Packet);
        case PACKET_DAMAGE_V2:              return (int)sizeof(DamageV2Packet);
        case PACKET_CAST_CANCEL:            return (int)sizeof(CastCancelPacket);
        case PACKET_ABILITY_CAST_START:     return (int)sizeof(AbilityCastStartPacket);
        case PACKET_ABILITY_EFFECT:         return (int)sizeof(AbilityEffectPacket);
        case PACKET_ABILITY_CAST_CANCEL:    return (int)sizeof(AbilityCastCancelPacket);
        case PACKET_MANA_UPDATE:            return (int)sizeof(ManaUpdatePacket);
        case PACKET_STATUS_EFFECT_APPLY:    return (int)sizeof(StatusEffectApplyPacket);
        case PACKET_STATUS_EFFECT_REMOVE:   return (int)sizeof(StatusEffectRemovePacket);
        case PACKET_SPAWN_ZONE:             return (int)sizeof(SpawnZonePacket);
        case PACKET_REMOVE_ZONE:            return (int)sizeof(RemoveZonePacket);
        case PACKET_NPC_POSITIONS:          return (int)sizeof(NPCPositionPacket);
        case PACKET_LEVEL_UP:               return (int)sizeof(LevelUpPacket);
        case PACKET_PLAYER_STATS:           return (int)sizeof(PlayerStatsPacket);
        default:                            return 0; // Unknown
    }
}

void network_update(void) {
    if (g_socket == INVALID_SOCKET || !g_connected) return;
    
    // Read as much as we can into the reassembly buffer
    while (1) {
        int space = (int)sizeof(g_recv_buf) - g_recv_len;
        if (space <= 0) break;  // Buffer full, process what we have
        
        int bytes = recv(g_socket, g_recv_buf + g_recv_len, space, 0);
        
        if (bytes > 0) {
            g_recv_len += bytes;
        } else if (bytes == 0) {
            g_connected = FALSE;
            closesocket(g_socket);
            g_socket = INVALID_SOCKET;
            g_recv_len = 0;
            return;
        } else {
            int err = WSAGetLastError();
            if (err == WSAEWOULDBLOCK) break;  // No more data right now
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
        
        // Need at least a header to determine packet type
        if (remaining < (int)sizeof(PacketHeader)) break;
        
        uint8_t pkt_type = (uint8_t)g_recv_buf[offset];
        int pkt_size = get_packet_size(pkt_type);
        
        if (pkt_size <= 0) {
            // Unknown packet type — try to use payload_size from header to skip
            PacketHeader* hdr = (PacketHeader*)(g_recv_buf + offset);
            uint16_t payload = ntohs(hdr->payload_size);
            if (payload > 0 && payload < 8192) {
                pkt_size = (int)sizeof(PacketHeader) + payload;
            } else {
                // Can't determine size — skip one byte and hope to resync
                printf("[NET] Unknown packet type %d, attempting resync\n", pkt_type);
                offset++;
                continue;
            }
        }
        
        // Do we have the full packet?
        if (remaining < pkt_size) break;  // Wait for more data
        
        // Process this single packet
        process_packet(g_recv_buf + offset, pkt_size);
        offset += pkt_size;
    }
    
    // Shift any leftover bytes to the front of the buffer
    if (offset > 0 && offset < g_recv_len) {
        memmove(g_recv_buf, g_recv_buf + offset, g_recv_len - offset);
        g_recv_len -= offset;
    } else if (offset >= g_recv_len) {
        g_recv_len = 0;
    }
}

void network_update_with_ping(int game_mode) {
    network_update();
    
    // Send pings during menu states
    if (game_mode == GAME_MODE_MAIN_MENU ||
        game_mode == GAME_MODE_SERVER_LIST ||
        game_mode == GAME_MODE_CHARACTER_SELECT) {
        
        double now = get_time();
        if (now - g_last_ping_time >= PING_INTERVAL) {
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
    printf("[NET] Connecting to realm %s:%u...\n", ip, port);
    
    g_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (g_socket == INVALID_SOCKET) return 0;
    
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    
    if (inet_pton(AF_INET, ip, &addr.sin_addr) <= 0) {
        closesocket(g_socket);
        g_socket = INVALID_SOCKET;
        return 0;
    }
    
    if (connect(g_socket, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        closesocket(g_socket);
        g_socket = INVALID_SOCKET;
        return 0;
    }
    
    // Set non-blocking
    u_long mode = 1;
    ioctlsocket(g_socket, FIONBIO, &mode);
    
    // Send connect packet
    RealmConnectPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.header.type = PACKET_REALM_CONNECT;
    pkt.header.player_id = htonl(account_id);
    memcpy(pkt.header.session_key, session_key, 32);
    
    send(g_socket, (char*)&pkt, sizeof(pkt), 0);
    
    // Wait for response
    fd_set fds;
    struct timeval tv = {10, 0};
    FD_ZERO(&fds);
    FD_SET(g_socket, &fds);
    
    if (select(0, &fds, NULL, NULL, &tv) <= 0) {
        closesocket(g_socket);
        g_socket = INVALID_SOCKET;
        return 0;
    }
    
    RealmConnectAckPacket ack;
    if (recv(g_socket, (char*)&ack, sizeof(ack), 0) <= 0) {
        closesocket(g_socket);
        g_socket = INVALID_SOCKET;
        return 0;
    }
    
    if (ack.success) {
        g_connected = TRUE;
        g_last_ping_time = get_time();
        printf("[NET] Connected to realm\n");
        return 1;
    }
    
    closesocket(g_socket);
    g_socket = INVALID_SOCKET;
    return 0;
}

int network_request_world_list(void) {
    if (!g_connected) return 0;
    
    g_world_list_ready = FALSE;
    
    WorldListRequestPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.header.type = PACKET_WORLD_LIST_REQUEST;
    pkt.header.player_id = htonl(g_account_id);
    
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

int network_request_enter_world(uint32_t character_id, uint32_t world_id) {
    if (!g_connected) return 0;
    
    g_enter_world_ready = FALSE;
    
    EnterWorldPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.header.type = PACKET_ENTER_WORLD;
    pkt.header.player_id = htonl(g_account_id);
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
    printf("[NET] Connecting to world %s:%u...\n", ip, port);
    
    // Close realm connection first
    if (g_socket != INVALID_SOCKET) {
        closesocket(g_socket);
    }
    
    g_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (g_socket == INVALID_SOCKET) return 0;
    
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    
    if (inet_pton(AF_INET, ip, &addr.sin_addr) <= 0) {
        closesocket(g_socket);
        g_socket = INVALID_SOCKET;
        return 0;
    }
    
    if (connect(g_socket, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        closesocket(g_socket);
        g_socket = INVALID_SOCKET;
        return 0;
    }
    
    // Set non-blocking
    u_long mode = 1;
    ioctlsocket(g_socket, FIONBIO, &mode);
    
    // Send world connect packet
    WorldConnectPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.header.type = PACKET_WORLD_CONNECT;
    pkt.header.player_id = htonl(g_account_id);
    memcpy(pkt.game_ticket, game_ticket, 64);
    pkt.character_id = htonl(character_id);
    
    send(g_socket, (char*)&pkt, sizeof(pkt), 0);
    
    // Wait for response
    fd_set fds;
    struct timeval tv = {5, 0};
    FD_ZERO(&fds);
    FD_SET(g_socket, &fds);
    
    if (select(0, &fds, NULL, NULL, &tv) <= 0) {
        closesocket(g_socket);
        g_socket = INVALID_SOCKET;
        return 0;
    }
    
    WorldConnectAckPacket ack;
    if (recv(g_socket, (char*)&ack, sizeof(ack), 0) <= 0) {
        closesocket(g_socket);
        g_socket = INVALID_SOCKET;
        return 0;
    }
    
    if (ack.header.type != PACKET_WORLD_CONNECT_ACK || !ack.success) {
        closesocket(g_socket);
        g_socket = INVALID_SOCKET;
        return 0;
    }
    
    g_connected = TRUE;
    g_last_ping_time = get_time();
    printf("[NET] Connected to world server\n");
    return 1;
}

int network_request_character_data(uint32_t character_id, uint32_t world_id) {
    if (!g_connected) return 0;
    
    g_char_data_ready = FALSE;
    
    WorldPlayerDataRequest pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.header.type = PACKET_REQUEST_PLAYER_DATA;
    pkt.header.player_id = htonl(g_account_id);
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
    
    PacketHeader pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.type = PACKET_PING;
    pkt.player_id = htonl(g_account_id);
    
    send(g_socket, (char*)&pkt, sizeof(pkt), 0);
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
    pkt.header.player_id = htonl(g_account_id);
    pkt.caster_id = htonl(g_account_id);
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

    send(g_socket, (char*)&pkt, sizeof(pkt), 0);
    printf("[NET] Requested player stats refresh\n");
}