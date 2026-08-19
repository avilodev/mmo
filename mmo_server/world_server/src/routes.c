/**
 * @file
 * Validate, rate-limit, and dispatch world-client packets to subsystem handlers.
 */

#include "ability_handler.h"
#include "log.h"
#include "packet_limiter.h"
#include "net_notify.h"
#include "connection_io.h"
#include "player_level.h"
#include "routes.h"
#include "combat.h"
#include "dialogue_handler.h"
#include "items_database.h"
#include "loot.h"
#include "shop.h"
#include "quest_system.h"

#include <math.h>
#include <string.h>
#include <arpa/inet.h>
#include <sys/socket.h>

static int valid_coord(float x, float y) {
    return isfinite(x) && isfinite(y) &&
           x > -100000.0f && x < 100000.0f &&
           y > -100000.0f && y < 100000.0f;
}

extern NPCWorld g_npc_world;

static void world_send_rate_limited(int fd, uint8_t rejected_type) {
    uint8_t buf[sizeof(RateLimitedPacket)];
    size_t n = net_build_rate_limited(buf, sizeof(buf), rejected_type,
                                      (uint8_t)packet_limiter_class_of(rejected_type),
                                      packet_limiter_retry_after_ms(fd, rejected_type));
    if (n) connection_io_send(fd, buf, n);
}

static void world_send_disconnect(int fd, uint8_t reason) {
    uint8_t buf[sizeof(DisconnectPacket)];
    size_t n = net_build_disconnect(buf, sizeof(buf), reason, NULL);
    if (n) connection_io_send(fd, buf, n);
}

/**
 * Dispatch one authenticated client packet.
 *
 * The player slot is an optional cache hint and is validated against character_id before use.
 *
 * @param client_fd  Client connection descriptor.
 * @param character_id  Authenticated character identifier.
 * @param player_slot  Cached active-player slot, or -1 when unavailable.
 * @param bytes  Number of received bytes in buffer.
 * @param buffer  Mutable packet bytes beginning with PacketHeader.
 * @return      1 to continue the connection, 0 when a packet is ignored, or -1 to disconnect.
 */
int process_packet(int client_fd, uint32_t character_id, int player_slot,
                   ssize_t bytes, uint8_t* buffer) {
    if (bytes < (ssize_t)sizeof(PacketHeader)) {
        LOG_WARN_RL(5, 60, "Packet too small (got %zd bytes, need at least %zu)", bytes, sizeof(PacketHeader));
        return 0;
    }

    PacketHeader* header = (PacketHeader*)buffer;

#ifdef DEBUG
    LOG_TRACE("Processing packet type: %d from character %u", header->type, character_id);
#endif

    // charge the packet budget before dispatch
    switch (packet_limiter_check(client_fd, header->type)) {
        case PACKET_LIMIT_DROP:
            if (packet_limiter_wants_rejection(header->type))
                world_send_rate_limited(client_fd, header->type);
            return 0;    // over budget: ignore, keep the connection open
        case PACKET_LIMIT_KICK:
            world_send_disconnect(client_fd, DISCONNECT_REASON_RATE_LIMIT);
            return -1;   // sustained abuse: caller closes the socket
        case PACKET_LIMIT_ALLOW:
        default:
            break;
    }

    // block state-changing actions while dead
    if (header->type == PACKET_PLAYER_MOVE ||
        header->type == PACKET_ATTACK_INTENT ||
        header->type == PACKET_ABILITY_CAST_INTENT ||
        header->type == PACKET_FORM_SWAP ||
        header->type == PACKET_EQUIP_ITEM ||
        header->type == PACKET_USE_ITEM ||
        header->type == PACKET_DROP_ITEM ||
        header->type == PACKET_NPC_INTERACT_REQUEST) {
        ActivePlayer* p = player_acquire_hint(character_id, player_slot);
        if (p) {
            int dead = p->is_dead;
            player_release(p);
            if (dead) return 1;
        }
    }

    switch (header->type) {
        case PACKET_LOGOUT:
            LOG_DEBUG("[LOGOUT] Character %u requested clean disconnect", character_id);
            return -1;  // Signal caller to break the recv loop cleanly

        case PACKET_PING:
            if (bytes >= (ssize_t)(sizeof(PacketHeader) + sizeof(uint16_t)))
                handle_ping(client_fd, buffer, character_id, player_slot);
            else
                LOG_WARN_RL(5, 60, "[PING] Malformed ping packet (size: %zd)", bytes);
            break;

        case PACKET_REQUEST_PLAYER_DATA:
            handle_request_player_data(client_fd, character_id);
            break;

        case PACKET_PLAYER_MOVE:
            if (bytes >= (ssize_t)sizeof(PlayerMovePacket)) {
                PlayerMovePacket* move = (PlayerMovePacket*)buffer;
                if (!valid_coord(move->pos_x, move->pos_y)) {
                    LOG_WARN_RL(5, 60, "[MOVE] Rejected: invalid coords from character %u", character_id);
                    break;
                }
                handle_player_move(client_fd, character_id, player_slot, move);
            } else {
                LOG_WARN_RL(5, 60, "[MOVE] Malformed move packet (size: %zd)", bytes);
            }
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

                uint32_t packet_char_id = ntohl(intent->header.player_id);
                if (packet_char_id != character_id) {
                    LOG_WARN_RL(5, 60, "[ATTACK] Character ID mismatch: packet=%u, session=%u", packet_char_id, character_id);
                    break;
                }

                if (!valid_coord(intent->aim_x, intent->aim_y)) {
                    LOG_WARN_RL(5, 60, "[ATTACK] Rejected: invalid aim coords from character %u", character_id);
                    break;
                }

                LOG_DEBUG("[ATTACK] Character %u attacking at (%.1f, %.1f)", character_id, intent->aim_x, intent->aim_y);

                combat_handle_attack_intent(&g_npc_world,
                                           client_fd,
                                           character_id,
                                           intent);
            } else {
                LOG_WARN_RL(5, 60, "[ATTACK] Malformed attack intent packet (size: %zd)", bytes);
            }
            break;
        }

        case PACKET_CAST_CANCEL: {
            LOG_DEBUG("[ATTACK] Character %u cancelled cast", character_id);
            combat_handle_cast_cancel(client_fd, character_id);
            break;
        }

        case PACKET_ABILITY_CAST_INTENT: {
            if (bytes >= (ssize_t)sizeof(AbilityCastIntentPacket)) {
                AbilityCastIntentPacket* intent = (AbilityCastIntentPacket*)buffer;

                uint32_t packet_char_id = ntohl(intent->header.player_id);
                if (packet_char_id != character_id) {
                    LOG_WARN_RL(5, 60, "[ABILITY] Character ID mismatch: packet=%u, session=%u", packet_char_id, character_id);
                    break;
                }

                if (!valid_coord(intent->aim_x, intent->aim_y)) {
                    LOG_WARN_RL(5, 60, "[ABILITY] Rejected: invalid aim coords from character %u", character_id);
                    break;
                }

                LOG_DEBUG("[ABILITY] Character %u casting ability %u at (%.1f, %.1f)", character_id, ntohs(intent->ability_id), intent->aim_x, intent->aim_y);

                ability_handle_cast_intent(&g_npc_world,
                                           client_fd,
                                           character_id,
                                           intent);
            } else {
                LOG_WARN_RL(5, 60, "[ABILITY] Malformed cast intent packet (size: %zd)", bytes);
            }
            break;
        }

        case PACKET_ABILITY_CAST_CANCEL: {
            LOG_DEBUG("[ABILITY] Character %u cancelled ability cast", character_id);
            ability_handle_cast_cancel(client_fd, character_id);
            break;
        }

        case PACKET_FORM_SWAP: {
            if (bytes < (ssize_t)sizeof(FormSwapPacket)) {
                LOG_WARN_RL(5, 60, "[FORM] Malformed form-swap packet (size: %zd)", bytes);
                break;
            }

            FormSwapPacket* swap = (FormSwapPacket*)buffer;
            uint32_t packet_char_id = ntohl(swap->header.player_id);
            if (packet_char_id != character_id) {
                LOG_WARN_RL(5, 60, "[FORM] Character ID mismatch: packet=%u, session=%u",
                            packet_char_id, character_id);
                break;
            }

            /* The requested form is validated inside the handler, which replies with
             * the authoritative state whether or not the swap is allowed. */
            ability_handle_form_swap(client_fd, character_id, swap->requested_form);
            break;
        }

       case PACKET_REQUEST_PLAYER_STATS: {
           ActivePlayer* player = player_acquire(character_id);
           if (player) {
               player_send_stats_locked(client_fd, player);
               // resynchronize abilities after entering play
               ability_send_data(client_fd, player);
               player_release(player);
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
                resp.header.type         = PACKET_LOOT_PICKUP_RESPONSE;
                resp.header.player_id    = htonl(character_id);
                resp.header.payload_size = htons(sizeof(LootPickupResponsePacket) - sizeof(PacketHeader));
                resp.ground_item_id = htonl(ground_item_id);

                const GroundItem* gi = loot_get_ground_item(ground_item_id);
                if (!gi) {
                    resp.success = 0;
                    strncpy(resp.message, "Item not found", sizeof(resp.message) - 1);
                    server_send(client_fd, &resp, sizeof(resp));
                    break;
                }

                ActivePlayer* player = player_acquire(character_id);
                if (!player) break;

                float dx = player->pos_x - gi->pos_x;
                float dy = player->pos_y - gi->pos_y;
                float dist = sqrtf(dx * dx + dy * dy);
                int inv_slot = inventory_first_free(player->inventory);
                player_release(player);

                if (dist > LOOT_PICKUP_RANGE) {
                    resp.success = 0;
                    strncpy(resp.message, "Too far away", sizeof(resp.message) - 1);
                    server_send(client_fd, &resp, sizeof(resp));
                    break;
                }

                if (inv_slot < 0) {
                    resp.success = 0;
                    strncpy(resp.message, "Inventory full", sizeof(resp.message) - 1);
                    server_send(client_fd, &resp, sizeof(resp));
                    break;
                }

                uint32_t item_id = 0;
                uint8_t quantity = 0;
                if (!loot_try_pickup(ground_item_id, character_id, &item_id, &quantity)) {
                    resp.success = 0;
                    strncpy(resp.message, "Cannot pick up yet", sizeof(resp.message) - 1);
                    server_send(client_fd, &resp, sizeof(resp));
                    break;
                }

                // reacquire after pickup releases player ownership
                player = player_acquire(character_id);
                uint16_t stored = 0;
                if (player && player->client_fd == client_fd) {
                    const ItemDefinition* def = item_get(item_id);
                    uint16_t want = quantity ? quantity : 1;
                    uint16_t left = inventory_add(player->inventory, item_id, want,
                                                  def ? def->max_stack : 1,
                                                  def ? def->bind_on_pickup : 0);
                    stored = (uint16_t)(want - left);
                    if (stored > 0) player->is_dirty = 1;
                }

                if (player && stored > 0) {
                    player_release(player);

                    quest_on_item_collect(character_id, client_fd, item_id);

                    resp.success = 1;
                    resp.item_id = htonl(item_id);
                    resp.quantity = (uint8_t)stored;
                    resp.inventory_slot = (uint8_t)(inv_slot < 0 ? 0 : inv_slot);
                    strncpy(resp.message, "Item picked up", sizeof(resp.message) - 1);
                } else {
                    if (player) player_release(player);
                    resp.success = 0;
                    strncpy(resp.message, "Player state changed", sizeof(resp.message) - 1);
                }
                server_send(client_fd, &resp, sizeof(resp));

                // refresh the stack that may have absorbed the pickup
                if (resp.success && inv_slot >= 0) {
                    uint16_t changed[1] = { (uint16_t)inv_slot };
                    player_send_slot_updates(client_fd, character_id, changed, 1);
                }
            }
            break;
        }

        case PACKET_SHOP_BUY:
            shop_handle_buy(character_id, client_fd, buffer, bytes);
            break;

        case PACKET_SHOP_SELL:
            shop_handle_sell(character_id, client_fd, buffer, bytes);
            break;

        case PACKET_SESSION_LIST_REQUEST:
            handle_session_list_request(client_fd, buffer, bytes);
            break;

        default: {
            char hex[16 * 3 + 1];
            int hex_len = 0;
            for (ssize_t _i = 0; _i < bytes && _i < 16; _i++)
                hex_len += snprintf(hex + hex_len, sizeof(hex) - (size_t)hex_len,
                                    "%02X ", buffer[_i]);
            if (hex_len == 0) hex[0] = '\0';
            LOG_WARN_RL(5, 60, "Unknown packet type: %d (0x%02X), size=%zd, bytes: %s",
                        header->type, header->type, bytes, hex);
            return 0;
        }
    }

    return 1;
}
