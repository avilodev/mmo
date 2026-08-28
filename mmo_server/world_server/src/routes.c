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
#include "friends.h"
#include "items_database.h"
#include "chat.h"
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

    /* Block state-changing actions while dead.
     *
     * The list started as the obvious four -- move, attack, cast, swap -- and
     * then grew by whichever handler someone happened to think of. Everything
     * a corpse should not be able to do belongs here, and the ones that were
     * missing were the ones that move items and money: a dead player could
     * pick loot off the ground, trade at a shop, unequip their gear and
     * rearrange their bags. Death is meant to interrupt exactly that.
     *
     * A single table rather than a chain of ||, so adding an opcode is adding
     * a line and forgetting one is visible. */
    static const uint8_t dead_blocked[] = {
        PACKET_PLAYER_MOVE,
        PACKET_ATTACK_INTENT,
        PACKET_ABILITY_CAST_INTENT,
        PACKET_FORM_SWAP,
        PACKET_EQUIP_ITEM,
        PACKET_UNEQUIP_ITEM,
        PACKET_MOVE_ITEM,
        PACKET_USE_ITEM,
        PACKET_DROP_ITEM,
        PACKET_LOOT_PICKUP_REQUEST,
        PACKET_SHOP_BUY,
        PACKET_SHOP_SELL,
        PACKET_NPC_INTERACT_REQUEST,
    };

    int blocked_while_dead = 0;
    for (size_t i = 0; i < sizeof(dead_blocked) / sizeof(dead_blocked[0]); i++) {
        if (header->type == dead_blocked[i]) { blocked_while_dead = 1; break; }
    }

    if (blocked_while_dead) {
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
                handle_ping(client_fd, buffer, bytes, character_id, player_slot);
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
            chat_handle_send(client_fd, character_id, buffer, bytes);
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

        case PACKET_LOOT_PICKUP_REQUEST:
            /* Implemented in loot.c, which owns the ground-item state. It used
             * to be written out inline here -- the one opcode implemented in
             * the router rather than in its module, and the one that destroyed
             * an item when the inventory insert it had already committed to
             * turned out to store nothing. */
            loot_handle_pickup_request(character_id, client_fd, buffer, bytes);
            break;

        case PACKET_QUEST_ABANDON:
            if (bytes >= (ssize_t)sizeof(QuestAbandonPacket)) {
                const QuestAbandonPacket* req = (const QuestAbandonPacket*)buffer;
                quest_player_abandon(character_id, client_fd, ntohl(req->quest_id));
            } else {
                LOG_WARN_RL(5, 60, "[QUEST] Malformed abandon packet (size: %zd)", bytes);
            }
            break;

        case PACKET_SHOP_BUY:
            shop_handle_buy(character_id, client_fd, buffer, bytes);
            break;

        case PACKET_SHOP_SELL:
            shop_handle_sell(character_id, client_fd, buffer, bytes);
            break;

        /* One handler for all five, because they differ only in which mutation
         * they queue and friends.c is where that mapping lives. Size checks and
         * the account lookup happen there too, per opcode. */
        case PACKET_FRIEND_REQUEST:
        case PACKET_FRIEND_RESPOND:
        case PACKET_FRIEND_REMOVE:
        case PACKET_FRIEND_BLOCK:
        case PACKET_FRIEND_LIST_REQUEST:
            world_friends_handle_packet(client_fd, character_id, buffer, bytes);
            break;

        case PACKET_SESSION_LIST_REQUEST:
            handle_session_list_request(client_fd, buffer, bytes);
            break;

        case PACKET_NAME_QUERY_REQUEST:
            handle_name_query_request(client_fd, buffer, bytes);
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
