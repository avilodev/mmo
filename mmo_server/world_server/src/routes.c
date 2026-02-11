#include "ability_handler.h"
#include "player_level.h"
#include "routes.h"
#include "combat.h"
#include "dialogue_handler.h"

extern NPCWorld g_npc_world;

int process_packet(int client_fd, uint32_t character_id, ssize_t bytes, uint8_t* buffer) {
    if (bytes < (ssize_t)sizeof(PacketHeader)) {
        printf("Packet too small (got %zd bytes, need at least %zu)\n", 
               bytes, sizeof(PacketHeader));
        return 0;
    }

    PacketHeader* header = (PacketHeader*)buffer;
    
    printf("Processing packet type: %d from character %u\n", header->type, character_id);

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

        default:
            printf("Unknown packet type: %d\n", header->type);
            return 0;
    }

    return 1;
}