/**
 * @file
 * Handle friend and presence packets, and send the five the client can raise.
 *
 * The fifth domain dispatcher alongside net_world.c and its siblings; see
 * net_internal.h for how they fit together.
 *
 * Nothing here holds state. Everything arriving lands in the friends panel,
 * which is where the client's copy of the friend graph lives, and everything
 * leaving is one packet built from a name the player supplied.
 */

#include "net_internal.h"
#include "ui/friends_panel.h"

#include <stdio.h>
#include <string.h>
#include <stddef.h>

/** Append one line to the chat log under a fixed sender.
 *
 * A friend coming online while the panel is shut is otherwise invisible, and
 * the chat log is where the client already reports things that happened. There
 * is no system channel on the wire, so this is a local-channel line under a
 * sender name of its own; nothing is sent to the server.
 */
static void friends_chat_notice(const char* text) {
    if (!g_current_game || !g_current_game->playing) return;

    ChatState* chat = &g_current_game->playing->chat;

    int idx = chat->line_count;
    if (idx >= MAX_CHAT_LINES) {
        memmove(&chat->lines[0], &chat->lines[1],
                sizeof(ChatLine) * (MAX_CHAT_LINES - 1));
        idx = MAX_CHAT_LINES - 1;
    } else {
        chat->line_count++;
    }

    snprintf(chat->lines[idx].sender, sizeof(chat->lines[idx].sender), "%s", "Friends");
    snprintf(chat->lines[idx].text, sizeof(chat->lines[idx].text), "%s", text);
    chat->lines[idx].channel = CHAT_CHANNEL_LOCAL;
}

/** Reach the friends panel, or NULL before a session exists. */
static FriendsState* friends_state(void) {
    if (!g_current_game || !g_current_game->playing) return NULL;
    return &g_current_game->playing->friends;
}

/**
 * Dispatch one friend packet.
 *
 * @param type  Opcode from the packet header.
 * @param data  Buffer holding one complete packet.
 * @param length  Available packet length in bytes.
 * @return 1 when this domain handled the opcode, otherwise 0.
 */
int net_dispatch_friends(uint8_t type, const char* data, int length) {
    FriendsState* fs = friends_state();

    switch (type) {
        case PACKET_FRIEND_LIST_RESPONSE: {
            /* The server trims this to the entries it filled, so only the base
             * is guaranteed; the panel checks the count against the bytes. */
            if (length < (int)offsetof(FriendListResponsePacket, friends)) return 1;
            if (fs) friends_panel_set_list(fs, (const FriendListResponsePacket*)data, length);
            NET_LOG("[NET] Friends list received\n");
            return 1;
        }

        case PACKET_FRIEND_REQUESTS_LIST: {
            if (length < (int)offsetof(FriendRequestsListPacket, requests)) return 1;
            if (fs) friends_panel_set_requests(fs, (const FriendRequestsListPacket*)data, length);
            NET_LOG("[NET] Friend requests received\n");
            return 1;
        }

        case PACKET_FRIEND_REQUEST_NOTIFY: {
            if (length < (int)sizeof(FriendRequestNotifyPacket)) return 1;
            const FriendRequestNotifyPacket* pkt = (const FriendRequestNotifyPacket*)data;

            char from[32];
            snprintf(from, sizeof(from), "%.31s", pkt->from_name);

            char line[96];
            snprintf(line, sizeof(line), "%s sent you a friend request. [U] to answer.",
                     from[0] ? from : "Someone");
            friends_chat_notice(line);
            if (fs) friends_panel_notice(fs, line);
            return 1;
        }

        case PACKET_FRIEND_PRESENCE_UPDATE: {
            if (length < (int)sizeof(FriendPresenceUpdatePacket)) return 1;
            const FriendPresenceUpdatePacket* pkt = (const FriendPresenceUpdatePacket*)data;

            if (fs) friends_panel_apply_presence(fs, pkt);

            /* Only the arrival is announced. A friend logging out is reported
             * by the panel's dot and nothing else -- a line per departure would
             * make a busy friends list into a scrolling log. */
            if (pkt->online) {
                char name[32];
                snprintf(name, sizeof(name), "%.31s", pkt->name);
                if (name[0]) {
                    char line[96];
                    snprintf(line, sizeof(line), "%s has come online.", name);
                    friends_chat_notice(line);
                }
            }
            return 1;
        }

        case PACKET_FRIEND_OP_RESULT: {
            if (length < (int)sizeof(FriendOpResultPacket)) return 1;
            const FriendOpResultPacket* pkt = (const FriendOpResultPacket*)data;

            char subject[32];
            snprintf(subject, sizeof(subject), "%.31s", pkt->subject_name);

            if (fs) friends_panel_set_result(fs, pkt->action, pkt->result, subject);

            /* Also into chat: the actions that produce these can be typed as
             * slash commands with the panel shut, and an answer nobody sees is
             * the same as no answer. */
            if (fs) friends_chat_notice(fs->notice);

            /* Anything that changed the graph invalidates the list on screen.
             * Asked for rather than patched here, because the server's answer
             * is the record and reconstructing it from an action code is how a
             * client's copy drifts. */
            if (pkt->result == FRIEND_WIRE_OK || pkt->result == FRIEND_WIRE_MUTUAL)
                network_send_friend_list_request();

            NET_LOG("[NET] Friend op %u result %u\n", pkt->action, pkt->result);
            return 1;
        }

        default:
            return 0;
    }
}

/* --- Sending ------------------------------------------------------------- */

/** Fill a header the same way every friend packet needs it. */
static void fill_header(PacketHeader* h, uint8_t type, size_t packet_size) {
    h->type = type;
    h->player_id = htonl(g_net.character_id);
    h->payload_size = htons((uint16_t)(packet_size - sizeof(PacketHeader)));
}

void network_send_friend_request(const char* target_name) {
    if (!g_net.connected || !target_name || !*target_name) return;

    FriendRequestPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    fill_header(&pkt.header, PACKET_FRIEND_REQUEST, sizeof(pkt));
    strncpy(pkt.target_name, target_name, sizeof(pkt.target_name) - 1);

    net_send((char*)&pkt, sizeof(pkt));
    NET_LOG("[NET] Friend request sent to: %s\n", target_name);
}

void network_send_friend_respond(const char* from_name, int accept) {
    if (!g_net.connected || !from_name || !*from_name) return;

    FriendRespondPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    fill_header(&pkt.header, PACKET_FRIEND_RESPOND, sizeof(pkt));
    strncpy(pkt.from_name, from_name, sizeof(pkt.from_name) - 1);
    pkt.accept = accept ? 1 : 0;

    net_send((char*)&pkt, sizeof(pkt));
    NET_LOG("[NET] Friend %s sent for: %s\n", accept ? "accept" : "decline", from_name);
}

void network_send_friend_remove(const char* target_name) {
    if (!g_net.connected || !target_name || !*target_name) return;

    FriendRemovePacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    fill_header(&pkt.header, PACKET_FRIEND_REMOVE, sizeof(pkt));
    strncpy(pkt.target_name, target_name, sizeof(pkt.target_name) - 1);

    net_send((char*)&pkt, sizeof(pkt));
    NET_LOG("[NET] Friend remove sent for: %s\n", target_name);
}

void network_send_friend_block(const char* target_name, int block) {
    if (!g_net.connected || !target_name || !*target_name) return;

    FriendBlockPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    fill_header(&pkt.header, PACKET_FRIEND_BLOCK, sizeof(pkt));
    strncpy(pkt.target_name, target_name, sizeof(pkt.target_name) - 1);
    pkt.block = block ? 1 : 0;

    net_send((char*)&pkt, sizeof(pkt));
    NET_LOG("[NET] Friend %s sent for: %s\n", block ? "block" : "unblock", target_name);
}

void network_send_friend_list_request(void) {
    if (!g_net.connected) return;

    FriendListRequestPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    fill_header(&pkt.header, PACKET_FRIEND_LIST_REQUEST, sizeof(pkt));

    net_send((char*)&pkt, sizeof(pkt));
    NET_LOG("[NET] Friends list requested\n");
}
