/**
 * @file
 * Handle session, account, character-selection, and connection packets.
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
#include "core/race_registry.h"
#include "audio/audio.h"
#include "ui/quest_log.h"
#include <stdio.h>
#include <string.h>
#include <stddef.h>
#include <math.h>

/**
 * Dispatch one session packet.
 *
 * @param type  Opcode from the packet header.
 * @param data  Buffer holding one complete packet.
 * @param length  Available packet length in bytes.
 * @return 1 when this domain handled the opcode, otherwise 0.
 */
int net_dispatch_session(uint8_t type, const char* data, int length) {
    (void)data; (void)length;

    switch (type) {
        case PACKET_PING:
            // Server echoed our ping back — measure RTT
            if (g_net.pending_pings > 0) {
                g_net.pending_pings--;
                double rtt = (net_now() - g_net.ping_send_time) * 1000.0;
                if (rtt > 0.0 && rtt < 60000.0)
                    g_net.ping_ms = (int)rtt;
            }
            break;

        case PACKET_WORLD_LIST_RESPONSE: {
            size_t base_size = offsetof(WorldListResponsePacket, worlds);

            if (length < (int)base_size) {
                NET_WARN("[NET] ❌ WORLD_LIST packet too small: %d < %zu\n", length, base_size);
                return 1;   // handled: the packet was rejected, not unrecognized
            }

            WorldListResponsePacket* pkt = (WorldListResponsePacket*)data;
            uint8_t claimed_count = pkt->count;

            size_t wire_size = base_size +
                               (size_t)claimed_count * sizeof(WorldInfo);
            if (length < (int)wire_size) {
                NET_WARN("[NET] WORLD_LIST incomplete: have %d bytes, need %zu\n",
                       length, wire_size);
                return 1;   // handled: the packet was rejected, not unrecognized
            }

            // Validate count is reasonable
            if (claimed_count > MAX_WORLDS) {
                NET_WARN("[NET] Server claims %d worlds, clamping to MAX_WORLDS=%d\n",
                       claimed_count, MAX_WORLDS);
                claimed_count = MAX_WORLDS;
            }

            size_t copy_size = base_size +
                               (size_t)claimed_count * sizeof(WorldInfo);

            EnterCriticalSection(&g_net.response_lock);
            memset(&g_net.world_list.data, 0, sizeof(WorldListResponsePacket));
            memcpy(&g_net.world_list.data, data, copy_size);
            g_net.world_list.data.count = claimed_count;

            for (int i = 0; i < claimed_count; i++) {
                g_net.world_list.data.worlds[i].name[63] = '\0';
                g_net.world_list.data.worlds[i].ip[15] = '\0';
                g_net.world_list.data.worlds[i].region[31] = '\0';

                // Debug: Print each world
                NET_LOG("[NET]   World %d: '%s' at %s:%d (status=%d)\n",
                       i,
                       g_net.world_list.data.worlds[i].name,
                       g_net.world_list.data.worlds[i].ip,
                       ntohs(g_net.world_list.data.worlds[i].port),
                       g_net.world_list.data.worlds[i].status);
            }

            g_net.world_list.ready = TRUE;
            LeaveCriticalSection(&g_net.response_lock);
            NET_LOG("[NET] ✓ World list received: %d worlds validated\n", claimed_count);
            break;
        }

        case PACKET_CHARACTER_LIST_RESPONSE: {
            size_t base_size = offsetof(CharacterListResponsePacket, characters);

            if (length < (int)base_size) {
                NET_WARN("[NET] ❌ CHAR_LIST packet too small: %d < %zu\n", length, base_size);
                return 1;   // handled: the packet was rejected, not unrecognized
            }

            CharacterListResponsePacket* pkt = (CharacterListResponsePacket*)data;
            uint8_t claimed_count = pkt->count;

            if (claimed_count > 10) {
                NET_WARN("[NET] Server claims %d characters, clamping to 10\n", claimed_count);
                claimed_count = 10;
            }

            size_t needed = base_size + (size_t)claimed_count * sizeof(pkt->characters[0]);
            if (length < (int)needed) {
                NET_WARN("[NET] CHAR_LIST incomplete: have %d bytes, need %zu\n",
                       length, needed);
                return 1;   // handled: the packet was rejected, not unrecognized
            }

            EnterCriticalSection(&g_net.response_lock);
            memset(&g_net.char_list.data, 0, sizeof(g_net.char_list.data));
            memcpy(&g_net.char_list.data, data, needed);
            g_net.char_list.data.count = claimed_count;

            // Null-terminate character names
            for (int i = 0; i < claimed_count; i++) {
                g_net.char_list.data.characters[i].name[31] = '\0';
            }

            g_net.char_list.ready = TRUE;
            LeaveCriticalSection(&g_net.response_lock);
            NET_LOG("[NET] ✓ Character list received: %d chars validated\n", claimed_count);
            break;
        }

        case PACKET_RACE_LIST_RESPONSE: {
            size_t base_size = offsetof(RaceListResponsePacket, races);
            if (length < (int)base_size) {
                NET_WARN("[NET] RACE_LIST packet too small: %d < %zu\n", length, base_size);
                return 1;   // handled: the packet was rejected, not unrecognized
            }

            RaceListResponsePacket* pkt = (RaceListResponsePacket*)data;
            uint8_t claimed_count = pkt->count;
            if (claimed_count > MAX_RACE_LIST) {
                NET_WARN("[NET] Server claims %d races, clamping to %d\n",
                       claimed_count, MAX_RACE_LIST);
                claimed_count = MAX_RACE_LIST;
            }

            size_t needed = base_size + (size_t)claimed_count * sizeof(pkt->races[0]);
            if (length < (int)needed) {
                NET_WARN("[NET] RACE_LIST incomplete: have %d bytes, need %zu\n", length, needed);
                return 1;   // handled: the packet was rejected, not unrecognized
            }

            EnterCriticalSection(&g_net.response_lock);
            memset(&g_net.race_list.data, 0, sizeof(g_net.race_list.data));
            memcpy(&g_net.race_list.data, data, needed);
            g_net.race_list.data.count = claimed_count;

            /* Terminate every string field: these are rendered directly. */
            for (int i = 0; i < claimed_count; i++) {
                RaceInfo* race = &g_net.race_list.data.races[i];
                race->key[sizeof(race->key) - 1] = '\0';
                race->name[sizeof(race->name) - 1] = '\0';
                race->latin[sizeof(race->latin) - 1] = '\0';
                race->passive_name[sizeof(race->passive_name) - 1] = '\0';
                race->passive_desc[sizeof(race->passive_desc) - 1] = '\0';
            }

            g_net.race_list.ready = TRUE;

            /* Kept, not just handed to the creation screen and dropped. The
             * client used to consume this once and then fall back to hardcoded
             * tables for every later use of a race -- colours, resource types
             * -- which is the client re-learning what the server had just told
             * it, wrongly, the moment races.json changed. */
            client_races_store(&g_net.race_list.data);

            LeaveCriticalSection(&g_net.response_lock);
            NET_LOG("[NET] Race list received: %d races\n", claimed_count);
            break;
        }

        case PACKET_FORM_SWAP_ACK:
            if (length >= (int)sizeof(FormSwapAckPacket)) {
                EnterCriticalSection(&g_net.response_lock);
                memcpy(&g_net.form_swap.data, data, sizeof(FormSwapAckPacket));
                g_net.form_swap.ready = TRUE;
                LeaveCriticalSection(&g_net.response_lock);
            }
            break;

        case PACKET_CHARACTER_CREATE_RESPONSE:
            if (length >= (int)sizeof(CharacterCreateResponsePacket)) {
                EnterCriticalSection(&g_net.response_lock);
                memcpy(&g_net.char_create.data, data, sizeof(CharacterCreateResponsePacket));
                g_net.char_create.data.character_name[31] = '\0';
                g_net.char_create.data.message[127] = '\0';
                g_net.char_create.ready = TRUE;
                LeaveCriticalSection(&g_net.response_lock);
            }
            break;

        case PACKET_CHARACTER_DELETE_RESPONSE:
            if (length >= (int)sizeof(CharacterDeleteResponsePacket)) {
                EnterCriticalSection(&g_net.response_lock);
                memcpy(&g_net.char_delete.data, data, sizeof(CharacterDeleteResponsePacket));
                g_net.char_delete.data.message[127] = '\0';
                g_net.char_delete.ready = TRUE;
                LeaveCriticalSection(&g_net.response_lock);
            }
            break;

        case PACKET_ENTER_WORLD_RESPONSE:
            if (length >= (int)sizeof(EnterWorldResponsePacket)) {
                EnterCriticalSection(&g_net.response_lock);
                memcpy(&g_net.enter_world.data, data, sizeof(EnterWorldResponsePacket));
                g_net.enter_world.data.game_ticket[63] = '\0';
                g_net.enter_world.data.world_ip[15] = '\0';
                g_net.enter_world.data.message[127] = '\0';
                g_net.enter_world.ready = TRUE;
                LeaveCriticalSection(&g_net.response_lock);
            }
            break;

        case PACKET_REALM_CONNECT_ACK:
            if (length >= (int)sizeof(RealmConnectAckPacket)) {
                EnterCriticalSection(&g_net.response_lock);
                memcpy(&g_net.realm_connect_ack.data, data, sizeof(RealmConnectAckPacket));
                g_net.realm_connect_ack.data.message[127] = '\0';
                g_net.realm_connect_ack.ready = TRUE;
                LeaveCriticalSection(&g_net.response_lock);
                NET_LOG("[NET] Realm connect ACK received: success=%d\n", g_net.realm_connect_ack.data.success);
            }
            break;

        case PACKET_WORLD_CONNECT_ACK:
            if (length >= (int)sizeof(WorldConnectAckPacket)) {
                EnterCriticalSection(&g_net.response_lock);
                memcpy(&g_net.world_connect_ack.data, data, sizeof(WorldConnectAckPacket));
                g_net.world_connect_ack.data.welcome_message[127] = '\0';
                g_net.world_connect_ack.ready = TRUE;
                LeaveCriticalSection(&g_net.response_lock);
                NET_LOG("[NET] World connect ACK received\n");
            }
            break;

        case PACKET_PLAYER_DATA_RESPONSE:
            if (length >= (int)sizeof(CharacterInfo)) {
                EnterCriticalSection(&g_net.response_lock);
                memcpy(&g_net.char_data.data, data, sizeof(CharacterInfo));
                g_net.char_data.data.name[31] = '\0';
                g_net.char_data.ready = TRUE;
                LeaveCriticalSection(&g_net.response_lock);

                // If game is already running, update live XP and balances immediately
                if (g_current_game && g_current_game->player.info_loaded) {
                    g_current_game->player.info.experience = mmo_ntohll(g_net.char_data.data.experience);
                    for (int c = 0; c < CURRENCY_COUNT; c++)
                        g_current_game->player.info.currency[c] =
                            ntohl(g_net.char_data.data.currency[c]);
                }

                NET_LOG("[NET] Character data received\n");
            }
            break;

        case PACKET_DISCONNECT: {
            // The server tells us why immediately before closing. Record it so
            // the UI can say what happened rather than falling back to the
            // ping-timeout path and reporting a generic "connection lost".
            EnterCriticalSection(&g_net.response_lock);
            g_net.disconnect.set    = TRUE;
            g_net.disconnect.reason = DISCONNECT_REASON_UNKNOWN;
            g_net.disconnect.message[0] = '\0';

            if (length >= (int)sizeof(DisconnectPacket)) {
                DisconnectPacket* pkt = (DisconnectPacket*)data;
                g_net.disconnect.reason = pkt->reason;
                memcpy(g_net.disconnect.message, pkt->message,
                       sizeof(g_net.disconnect.message) - 1);
                g_net.disconnect.message[sizeof(g_net.disconnect.message) - 1] = '\0';
            }
            LeaveCriticalSection(&g_net.response_lock);

            NET_LOG("[NET] Server closed connection: reason=%u %s\n",
                   g_net.disconnect.reason, g_net.disconnect.message);
            g_net.connected = FALSE;
            break;
        }

        case PACKET_RATE_LIMITED: {
            // reported throttling excludes supersedable traffic
            if (length >= (int)sizeof(RateLimitedPacket)) {
                RateLimitedPacket* pkt = (RateLimitedPacket*)data;
                EnterCriticalSection(&g_net.response_lock);
                g_net.rate_limit.ready          = TRUE;
                g_net.rate_limit.rejected_type  = pkt->rejected_type;
                g_net.rate_limit.limit_class    = pkt->limit_class;
                g_net.rate_limit.retry_after_ms = ntohs(pkt->retry_after_ms);
                LeaveCriticalSection(&g_net.response_lock);

                NET_WARN("[NET] Request type %u rate limited, retry in %ums\n",
                       pkt->rejected_type, ntohs(pkt->retry_after_ms));
            }
            break;
        }

        case PACKET_SESSION_LIST_RESPONSE: {
            // Server sends a variable-length packet: fixed header + count entries.
            // Minimum needed: up to (but not including) the entries array.
            int sl_base = (int)offsetof(SessionListResponsePacket, entries);
            if (length < sl_base || !g_current_game || !g_current_game->playing) break;

            SessionListResponsePacket* pkt = (SessionListResponsePacket*)data;
            uint8_t count = pkt->count;
            if (count > SESSION_LIST_PAGE_SIZE) count = SESSION_LIST_PAGE_SIZE;

            // Validate the packet actually contains all claimed entries
            int sl_needed = sl_base + (int)count * (int)sizeof(SessionPlayerEntry);
            if (length < sl_needed) {
                NET_WARN("[NET] SESSION_LIST incomplete: have %d bytes, need %d for %d entries\n",
                       length, sl_needed, count);
                break;
            }

            g_current_game->playing->session_total_players = ntohl(pkt->total_players);
            g_current_game->playing->session_total_pages   = ntohs(pkt->total_pages);
            g_current_game->playing->session_current_page  = ntohs(pkt->current_page);
            g_current_game->playing->session_list_count    = (int)count;

            for (int i = 0; i < (int)count; i++) {
                SessionPlayerEntry* src = &pkt->entries[i];
                SessionPlayer*      dst = &g_current_game->playing->session_list[i];
                dst->player_id    = ntohl(src->player_id);
                src->name[31]     = '\0';
                memcpy(dst->name, src->name, 32);
                dst->level        = src->level;
                dst->player_class = src->player_class;
                dst->player_race  = src->player_race;
                dst->ping_ms      = ntohs(src->ping_ms);
            }
            NET_LOG("[NET] Session list: page %u/%u, %u total players, %d on page\n",
                   g_current_game->playing->session_current_page + 1,
                   g_current_game->playing->session_total_pages,
                   g_current_game->playing->session_total_players,
                   (int)count);
            break;
        }

        default:
            return 0;   // not ours; the next domain gets a look
    }

    return 1;
}

/* --- Session, account, and character-selection requests ------------------ */


/**
 * Send a world-list request and clear any prior response.
 *
 * @return      Nonzero when the complete request is sent; otherwise zero.
 */
int network_request_world_list(void) {
    if (!g_net.connected) return 0;

    EnterCriticalSection(&g_net.response_lock);
    g_net.world_list.ready = FALSE;
    LeaveCriticalSection(&g_net.response_lock);

    WorldListRequestPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.header.type = PACKET_WORLD_LIST_REQUEST;
    pkt.header.player_id = htonl(g_net.account_id);
    pkt.header.payload_size = htons(sizeof(WorldListRequestPacket) - sizeof(PacketHeader));

    return net_send((char*)&pkt, sizeof(pkt)) == sizeof(pkt);
}

/**
 * Consume the pending world-list response.
 *
 * @return      Nonzero when a response is copied; otherwise zero.
 */
int network_get_world_list(WorldListResponsePacket* out) {
    EnterCriticalSection(&g_net.response_lock);
    if (!g_net.world_list.ready) {
        LeaveCriticalSection(&g_net.response_lock);
        return 0;
    }
    memcpy(out, &g_net.world_list.data, sizeof(WorldListResponsePacket));
    g_net.world_list.ready = FALSE;
    LeaveCriticalSection(&g_net.response_lock);
    return 1;
}

/**
 * Request the account's characters for a world.
 *
 * @return      Nonzero when the complete request is sent; otherwise zero.
 */
int network_request_character_list(uint32_t world_id) {
    if (!g_net.connected) return 0;

    EnterCriticalSection(&g_net.response_lock);
    g_net.char_list.ready = FALSE;
    LeaveCriticalSection(&g_net.response_lock);

    CharacterListRequestPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.header.type = PACKET_CHARACTER_LIST_REQUEST;
    pkt.header.player_id = htonl(g_net.account_id);
    pkt.header.payload_size = htons(sizeof(CharacterListRequestPacket) - sizeof(PacketHeader));
    pkt.world_id = htonl(world_id);

    return net_send((char*)&pkt, sizeof(pkt)) == sizeof(pkt);
}

/**
 * Ask the realm server which races exist.
 *
 * The client keeps no race table, so the creation screen cannot be drawn until this
 * answers — which is what lets a new race reach the player with no client change.
 *
 * @return Nonzero when the request is sent; otherwise zero.
 */
int network_request_race_list(void) {
    if (!g_net.connected) return 0;

    EnterCriticalSection(&g_net.response_lock);
    g_net.race_list.ready = FALSE;
    LeaveCriticalSection(&g_net.response_lock);

    RaceListRequestPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.header.type = PACKET_RACE_LIST_REQUEST;
    pkt.header.player_id = htonl(g_net.account_id);
    pkt.header.payload_size = htons(sizeof(RaceListRequestPacket) - sizeof(PacketHeader));

    return net_send((char*)&pkt, sizeof(pkt)) == sizeof(pkt);
}

/**
 * Consume the pending race-list response.
 *
 * @return Nonzero when a response is copied; otherwise zero.
 */
int network_get_race_list(RaceListResponsePacket* out) {
    EnterCriticalSection(&g_net.response_lock);
    if (!g_net.race_list.ready) {
        LeaveCriticalSection(&g_net.response_lock);
        return 0;
    }
    memcpy(out, &g_net.race_list.data, sizeof(RaceListResponsePacket));
    g_net.race_list.ready = FALSE;
    LeaveCriticalSection(&g_net.response_lock);
    return 1;
}

/**
 * Ask the world server to swap forms.
 *
 * @return Nonzero when the request is sent; otherwise zero.
 */
int network_request_form_swap(uint8_t requested_form) {
    if (!g_net.connected) return 0;

    FormSwapPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.header.type = PACKET_FORM_SWAP;
    pkt.header.player_id = htonl(g_net.character_id);
    pkt.header.payload_size = htons(sizeof(FormSwapPacket) - sizeof(PacketHeader));
    pkt.requested_form = requested_form;

    return net_send((char*)&pkt, sizeof(pkt)) == sizeof(pkt);
}

/**
 * Consume the pending form-swap acknowledgement.
 *
 * The server replies whether or not the swap was allowed, so a refusal arrives here
 * too and carries the form the character is authoritatively still in.
 *
 * @return Nonzero when an acknowledgement is copied; otherwise zero.
 */
int network_get_form_swap_ack(FormSwapAckPacket* out) {
    EnterCriticalSection(&g_net.response_lock);
    if (!g_net.form_swap.ready) {
        LeaveCriticalSection(&g_net.response_lock);
        return 0;
    }
    memcpy(out, &g_net.form_swap.data, sizeof(FormSwapAckPacket));
    g_net.form_swap.ready = FALSE;
    LeaveCriticalSection(&g_net.response_lock);
    return 1;
}

/**
 * Consume the pending character-list response.
 *
 * @return      Nonzero when a response is copied; otherwise zero.
 */
int network_get_character_list(CharacterListResponsePacket* out) {
    EnterCriticalSection(&g_net.response_lock);
    if (!g_net.char_list.ready) {
        LeaveCriticalSection(&g_net.response_lock);
        return 0;
    }
    memcpy(out, &g_net.char_list.data, sizeof(CharacterListResponsePacket));
    g_net.char_list.ready = FALSE;
    LeaveCriticalSection(&g_net.response_lock);
    return 1;
}

/**
 * Send a character-creation request.
 *
 * @param name  NUL-terminated character name, truncated to 31 bytes on the wire.
 * @return      Nonzero when the complete request is sent; otherwise zero.
 */
int network_create_character(uint32_t world_id, const char* name,
                            uint32_t class_id, uint32_t race_id) {
    if (!g_net.connected) return 0;

    EnterCriticalSection(&g_net.response_lock);
    g_net.char_create.ready = FALSE;
    LeaveCriticalSection(&g_net.response_lock);

    CharacterCreateRequestPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.header.type = PACKET_CHARACTER_CREATE_REQUEST;
    pkt.header.player_id = htonl(g_net.account_id);
    pkt.header.payload_size = htons(sizeof(CharacterCreateRequestPacket) - sizeof(PacketHeader));
    pkt.world_id = htonl(world_id);
    strncpy(pkt.name, name, 31);
    pkt.class_id = htonl(class_id);
    pkt.race_id = htonl(race_id);

    return net_send((char*)&pkt, sizeof(pkt)) == sizeof(pkt);
}

/**
 * Consume the pending character-creation response.
 *
 * @return      Nonzero when a response is copied; otherwise zero.
 */
int network_get_character_create_response(CharacterCreateResponsePacket* out) {
    EnterCriticalSection(&g_net.response_lock);
    if (!g_net.char_create.ready) {
        LeaveCriticalSection(&g_net.response_lock);
        return 0;
    }
    memcpy(out, &g_net.char_create.data, sizeof(CharacterCreateResponsePacket));
    g_net.char_create.ready = FALSE;
    LeaveCriticalSection(&g_net.response_lock);
    return 1;
}

/**
 * Send a character-deletion request.
 *
 * @return      Nonzero when the complete request is sent; otherwise zero.
 */
int network_delete_character(uint32_t world_id, uint32_t character_id) {
    if (!g_net.connected) return 0;

    EnterCriticalSection(&g_net.response_lock);
    g_net.char_delete.ready = FALSE;
    LeaveCriticalSection(&g_net.response_lock);

    CharacterDeleteRequestPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.header.type = PACKET_CHARACTER_DELETE_REQUEST;
    pkt.header.player_id = htonl(g_net.account_id);
    pkt.header.payload_size = htons(sizeof(CharacterDeleteRequestPacket) - sizeof(PacketHeader));
    pkt.character_id = htonl(character_id);
    pkt.world_id = htonl(world_id);

    return net_send((char*)&pkt, sizeof(pkt)) == sizeof(pkt);
}

/**
 * Consume the pending character-deletion response.
 *
 * @return      Nonzero when a response is copied; otherwise zero.
 */
int network_get_character_delete_response(CharacterDeleteResponsePacket* out) {
    EnterCriticalSection(&g_net.response_lock);
    if (!g_net.char_delete.ready) {
        LeaveCriticalSection(&g_net.response_lock);
        return 0;
    }
    memcpy(out, &g_net.char_delete.data, sizeof(CharacterDeleteResponsePacket));
    g_net.char_delete.ready = FALSE;
    LeaveCriticalSection(&g_net.response_lock);
    return 1;
}

/**
 * Request a game ticket and endpoint for entering a world.
 *
 * @return      Nonzero when the complete request is sent; otherwise zero.
 */
int network_request_enter_world(uint32_t character_id, uint32_t world_id) {
    if (!g_net.connected) return 0;

    EnterCriticalSection(&g_net.response_lock);
    g_net.enter_world.ready = FALSE;
    LeaveCriticalSection(&g_net.response_lock);

    EnterWorldPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.header.type = PACKET_ENTER_WORLD;
    pkt.header.player_id = htonl(g_net.account_id);
    pkt.header.payload_size = htons(sizeof(EnterWorldPacket) - sizeof(PacketHeader));
    pkt.character_id = htonl(character_id);
    pkt.world_id = htonl(world_id);

    NET_LOG("[NET] Requesting enter world (char %u, world %u)\n", character_id, world_id);
    return net_send((char*)&pkt, sizeof(pkt)) == sizeof(pkt);
}

/**
 * Consume the pending enter-world response.
 *
 * @return      Nonzero when a response is copied; otherwise zero.
 */
int network_get_enter_world_response(EnterWorldResponsePacket* out) {
    EnterCriticalSection(&g_net.response_lock);
    if (!g_net.enter_world.ready) {
        LeaveCriticalSection(&g_net.response_lock);
        return 0;
    }
    memcpy(out, &g_net.enter_world.data, sizeof(EnterWorldResponsePacket));
    g_net.enter_world.ready = FALSE;
    LeaveCriticalSection(&g_net.response_lock);
    return 1;
}


/**
 * Request a full character record from the world server.
 *
 * @return      Nonzero when the complete request is sent; otherwise zero.
 */
int network_request_character_data(uint32_t character_id, uint32_t world_id) {
    if (!g_net.connected) return 0;

    EnterCriticalSection(&g_net.response_lock);
    g_net.char_data.ready = FALSE;
    LeaveCriticalSection(&g_net.response_lock);

    WorldPlayerDataRequest pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.header.type = PACKET_REQUEST_PLAYER_DATA;
    pkt.header.player_id = htonl(g_net.account_id);
    pkt.header.payload_size = htons(sizeof(WorldPlayerDataRequest) - sizeof(PacketHeader));
    pkt.character_id = htonl(character_id);
    pkt.world_id = htonl(world_id);

    return net_send((char*)&pkt, sizeof(pkt)) == sizeof(pkt);
}

/**
 * Consume character data and convert its integer fields to host byte order.
 *
 * Inventory and equipment entries are converted in place exactly once by this function.
 *
 * @return      Nonzero when a response is copied and converted; otherwise zero.
 */
int network_get_character_data(CharacterInfo* out) {
    EnterCriticalSection(&g_net.response_lock);
    if (!g_net.char_data.ready) {
        LeaveCriticalSection(&g_net.response_lock);
        return 0;
    }
    memcpy(out, &g_net.char_data.data, sizeof(CharacterInfo));
    g_net.char_data.ready = FALSE;
    LeaveCriticalSection(&g_net.response_lock);

    // Convert from network byte order
    out->level = ntohl(out->level);
    out->health = ntohl(out->health);
    out->max_health = ntohl(out->max_health);
    out->resource = (int32_t)ntohl((uint32_t)out->resource);
    out->max_resource = (int32_t)ntohl((uint32_t)out->max_resource);
    out->experience = mmo_ntohll(out->experience);

    /* Race and class fuse into one identifier; resource_type and form are single
     * bytes and need no conversion. */
    out->race_id = ntohl(out->race_id);

    for (int c = 0; c < CURRENCY_COUNT; c++)
        out->currency[c] = ntohl(out->currency[c]);

    // preserve 64-bit instance identifiers during conversion
    for (int i = 0; i < INVENTORY_SLOT_COUNT; i++) {
        out->inventory[i].instance_id = mmo_ntohll(out->inventory[i].instance_id);
        out->inventory[i].item_id     = ntohl(out->inventory[i].item_id);
        out->inventory[i].quantity    = ntohs(out->inventory[i].quantity);
    }
    for (int i = 0; i < EQUIP_SLOTS; i++) {
        out->equipment[i].instance_id = mmo_ntohll(out->equipment[i].instance_id);
        out->equipment[i].item_id     = ntohl(out->equipment[i].item_id);
        out->equipment[i].quantity    = ntohs(out->equipment[i].quantity);
    }

    return 1;
}

/**
 * Request one zero-based page of the active-player session list.
 */
void network_send_session_list_request(uint16_t page) {
    if (!g_net.connected) return;

    SessionListRequestPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.header.type = PACKET_SESSION_LIST_REQUEST;
    pkt.header.player_id = htonl(g_net.character_id);
    pkt.header.payload_size = htons(sizeof(SessionListRequestPacket) - sizeof(PacketHeader));
    pkt.page = htons(page);

    net_send((char*)&pkt, sizeof(pkt));
    NET_LOG("[NET] Session list request: page=%u\n", page);
}
