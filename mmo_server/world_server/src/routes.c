#include "ability_handler.h"
#include "player_level.h"
#include "routes.h"
#include "combat.h"
#include "dialogue_handler.h"
#include "loot.h"

#include <math.h>
#include <string.h>
#include <arpa/inet.h>
#include <sys/socket.h>

// Reject obviously bad float coordinates from clients (#18)
static int valid_coord(float x, float y) {
    return isfinite(x) && isfinite(y) &&
           x > -100000.0f && x < 100000.0f &&
           y > -100000.0f && y < 100000.0f;
}

extern NPCWorld g_npc_world;

int process_packet(int client_fd, uint32_t character_id, ssize_t bytes, uint8_t* buffer) {
    if (bytes < (ssize_t)sizeof(PacketHeader)) {
        printf("Packet too small (got %zd bytes, need at least %zu)\n", 
               bytes, sizeof(PacketHeader));
        return 0;
    }

    PacketHeader* header = (PacketHeader*)buffer;
    
    printf("Processing packet type: %d from character %u\n", header->type, character_id);

    // Reject most actions from dead players
    if (header->type == PACKET_PLAYER_MOVE ||
        header->type == PACKET_ATTACK_INTENT ||
        header->type == PACKET_ABILITY_CAST_INTENT ||
        header->type == PACKET_EQUIP_ITEM ||
        header->type == PACKET_USE_ITEM ||
        header->type == PACKET_DROP_ITEM ||
        header->type == PACKET_NPC_INTERACT_REQUEST) {
        ActivePlayer* p = player_find_active(character_id);
        if (p) {
            pthread_mutex_lock(&p->lock);
            int dead = p->is_dead;
            pthread_mutex_unlock(&p->lock);
            if (dead) return 1; // Silently ignore
        }
    }

    // Route to appropriate handler
    switch (header->type) {
        case PACKET_PING:
            handle_ping(client_fd, buffer);
            break;

        case PACKET_REQUEST_PLAYER_DATA:
            handle_request_player_data(client_fd, character_id);
            break;

        case PACKET_PLAYER_MOVE:
            handle_player_move(client_fd, character_id, (PlayerMovePacket*)buffer);
            break;
            
        case PACKET_EQUIP_ITEM:
            handle_equip_item(client_fd, character_id, buffer, bytes);
            break; 
            
        case PACKET_UNEQUIP_ITEM:
            handle_unequip_item(client_fd, character_id, buffer, bytes);
            break;
            
        case PACKET_USE_ITEM:
            handle_use_item(client_fd, character_id, buffer, bytes);
            break;
            
        case PACKET_DROP_ITEM:
            handle_drop_item(client_fd, character_id, buffer, bytes);
            break;
            
        case PACKET_MOVE_ITEM:
            handle_move_item(client_fd, character_id, buffer, bytes);
            break;

        case PACKET_ATTACK_INTENT: {
            if (bytes >= (ssize_t)sizeof(AttackIntentPacket)) {
                AttackIntentPacket* intent = (AttackIntentPacket*)buffer;
                
                // Validate packet is for this character
                uint32_t packet_char_id = ntohl(intent->header.player_id);
                if (packet_char_id != character_id) {
                    printf("[ATTACK] Character ID mismatch: packet=%u, session=%u\n",
                           packet_char_id, character_id);
                    break;
                }
                
                if (!valid_coord(intent->aim_x, intent->aim_y)) {
                    printf("[ATTACK] Rejected: invalid aim coords from character %u\n",
                           character_id);
                    break;
                }

                printf("[ATTACK] Character %u attacking at (%.1f, %.1f)\n",
                       character_id, intent->aim_x, intent->aim_y);

                combat_handle_attack_intent(&g_npc_world,
                                           client_fd,
                                           character_id,
                                           intent);
            } else {
                printf("[ATTACK] Malformed attack intent packet (size: %zd)\n", bytes);
            }
            break;
        }
            
        case PACKET_CAST_CANCEL: {
            printf("[ATTACK] Character %u cancelled cast\n", character_id);
            combat_handle_cast_cancel(client_fd, character_id);
            break;
        }

        case PACKET_ABILITY_CAST_INTENT: {
            if (bytes >= (ssize_t)sizeof(AbilityCastIntentPacket)) {
                AbilityCastIntentPacket* intent = (AbilityCastIntentPacket*)buffer;

                uint32_t packet_char_id = ntohl(intent->header.player_id);
                if (packet_char_id != character_id) {
                    printf("[ABILITY] Character ID mismatch: packet=%u, session=%u\n",
                           packet_char_id, character_id);
                    break;
                }

                if (!valid_coord(intent->aim_x, intent->aim_y)) {
                    printf("[ABILITY] Rejected: invalid aim coords from character %u\n",
                           character_id);
                    break;
                }

                printf("[ABILITY] Character %u casting ability %u at (%.1f, %.1f)\n",
                       character_id, ntohs(intent->ability_id),
                       intent->aim_x, intent->aim_y);

                ability_handle_cast_intent(&g_npc_world,
                                           client_fd,
                                           character_id,
                                           intent);
            } else {
                printf("[ABILITY] Malformed cast intent packet (size: %zd)\n", bytes);
            }
            break;
        }

        case PACKET_ABILITY_CAST_CANCEL: {
            printf("[ABILITY] Character %u cancelled ability cast\n", character_id);
            ability_handle_cast_cancel(client_fd, character_id);
            break;
        }

       case PACKET_REQUEST_PLAYER_STATS: {
           ActivePlayer* player = player_find_active(character_id);
           if (player) {
               player_send_stats(client_fd, player);
           }
           break;
       }

        case PACKET_NPC_INTERACT_REQUEST:
            handle_npc_interact_request(client_fd, character_id, buffer, bytes);
            break;

        case PACKET_DIALOGUE_OPTION_SELECT:
            handle_dialogue_option_select(client_fd, character_id, buffer, bytes);
            break;

        case PACKET_DIALOGUE_CLOSE:
            handle_dialogue_close(character_id);
            break;

        case PACKET_CHAT_SEND:
            handle_chat_send(client_fd, character_id, buffer, bytes);
            break;

        case PACKET_PARTY_INVITE:
            handle_party_invite(client_fd, character_id, buffer, bytes);
            break;

        case PACKET_PARTY_ACCEPT:
            handle_party_accept(client_fd, character_id);
            break;

        case PACKET_PARTY_DECLINE:
            handle_party_decline(client_fd, character_id);
            break;

        case PACKET_PARTY_LEAVE:
            handle_party_leave(client_fd, character_id);
            break;

        case PACKET_PARTY_KICK:
            handle_party_kick(client_fd, character_id, buffer, bytes);
            break;

        case PACKET_LOOT_PICKUP_REQUEST: {
            if (bytes >= (ssize_t)sizeof(LootPickupRequestPacket)) {
                LootPickupRequestPacket* req = (LootPickupRequestPacket*)buffer;
                uint32_t ground_item_id = ntohl(req->ground_item_id);

                LootPickupResponsePacket resp = {0};
                resp.header.type = PACKET_LOOT_PICKUP_RESPONSE;
                resp.header.player_id = htonl(character_id);
                resp.ground_item_id = htonl(ground_item_id);

                // Distance check
                const GroundItem* gi = loot_get_ground_item(ground_item_id);
                if (!gi) {
                    resp.success = 0;
                    strncpy(resp.message, "Item not found", sizeof(resp.message) - 1);
                    send(client_fd, &resp, sizeof(resp), 0);
                    break;
                }

                ActivePlayer* player = player_find_active(character_id);
                if (!player) break;

                pthread_mutex_lock(&player->lock);
                float dx = player->pos_x - gi->pos_x;
                float dy = player->pos_y - gi->pos_y;
                float dist = sqrtf(dx * dx + dy * dy);
                pthread_mutex_unlock(&player->lock);

                if (dist > LOOT_PICKUP_RANGE) {
                    resp.success = 0;
                    strncpy(resp.message, "Too far away", sizeof(resp.message) - 1);
                    send(client_fd, &resp, sizeof(resp), 0);
                    break;
                }

                uint32_t item_id = 0;
                uint8_t quantity = 0;
                if (!loot_try_pickup(ground_item_id, character_id, &item_id, &quantity)) {
                    resp.success = 0;
                    strncpy(resp.message, "Cannot pick up yet", sizeof(resp.message) - 1);
                    send(client_fd, &resp, sizeof(resp), 0);
                    break;
                }

                // Add to player inventory
                pthread_mutex_lock(&player->lock);
                int inv_slot = -1;
                for (int s = 0; s < 150; s++) {
                    if (player->inventory[s] == 0) {
                        inv_slot = s;
                        break;
                    }
                }
                if (inv_slot >= 0) {
                    player->inventory[inv_slot] = item_id;
                    player->is_dirty = 1;
                    pthread_mutex_unlock(&player->lock);

                    resp.success = 1;
                    resp.item_id = htonl(item_id);
                    resp.quantity = quantity;
                    resp.inventory_slot = (uint8_t)inv_slot;
                    strncpy(resp.message, "Item picked up", sizeof(resp.message) - 1);
                } else {
                    pthread_mutex_unlock(&player->lock);
                    resp.success = 0;
                    strncpy(resp.message, "Inventory full", sizeof(resp.message) - 1);
                }
                send(client_fd, &resp, sizeof(resp), 0);
            }
            break;
        }

        default:
            printf("Unknown packet type: %d\n", header->type);
            return 0;
    }

    return 1;
}