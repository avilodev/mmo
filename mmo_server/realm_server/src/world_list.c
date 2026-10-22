/**
 * @file
 * Send realm world listings and issue single-use world-entry tickets.
 */
#include "world_list.h"

#include "realm_service.h"
#include "peer_addr.h"
#include "log.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/random.h>
#include <time.h>
#include <stddef.h>
#include <stdint.h>
#include "tls.h"

/** Narrow a 32-bit count to the 16-bit wire field without wrapping. */
static uint16_t clamp_u16(uint32_t value) {
    return value > UINT16_MAX ? UINT16_MAX : (uint16_t)value;
}

/**
 * Snapshot monitored world status and send it to an authenticated realm client.
 *
 * World identifiers, population values, capacities, and ports are encoded in network byte order.
 */
void world_send_list(int client_fd, uint32_t account_id) {
    WorldListResponsePacket response;
    memset(&response, 0, sizeof(response));
    response.header.type = PACKET_WORLD_LIST_RESPONSE;
    response.header.player_id = htonl(account_id);
    
    pthread_mutex_lock(&g_server.world_servers_lock);

    /* Clamped BEFORE it is assigned, not after.
     *
     * The fill loop below has always stopped at MAX_WORLDS, but `count` was
     * assigned the unclamped roster size and send_size is computed from it --
     * so a roster larger than the packet could hold made the realm send, and
     * the client read, past the end of the worlds array. The roster is now
     * sized from worlds.conf at runtime, so "more worlds than the packet
     * carries" is a configuration away rather than impossible. */
    int listed = g_server.num_world_servers;
    if (listed > MAX_WORLDS) {
        listed = MAX_WORLDS;
        LOG_WARN("World list holds %d worlds; only the first %d fit in one "
                 "response packet", g_server.num_world_servers, MAX_WORLDS);
    }
    response.count = (uint8_t)listed;

    // Copy live world server data
    for (int i = 0; i < listed; i++) {
        WorldServer* ws = &g_server.world_servers[i];
        WorldInfo* world = &response.worlds[i];
        
        world->world_id = htonl(i + 1);
        
        // FIX: Properly handle all string fields
        strncpy(world->name, ws->name, sizeof(world->name) - 1);
        world->name[sizeof(world->name) - 1] = '\0';
        
        /* Clamped, not truncated. Both wire fields are 16 bits and both
         * sources are 32, so a world configured above 65535 used to publish
         * the low half of its number -- a capacity of 100000 arrived as 34464,
         * which reads as a plausible answer rather than as a wrong one. */
        world->population = htons(clamp_u16(ws->player_count));
        world->capacity   = htons(clamp_u16(ws->max_players));

        /* Determine status based on player count vs max.
         *
         * max_players is 0 until the world's first status heartbeat lands, and
         * an unknown capacity is not a capacity of zero -- without the guard a
         * freshly connected world reports itself full at 0 >= 0. */
        if (!ws->online) {
            world->status = 0; // offline
        } else if (ws->max_players > 0 && ws->player_count >= ws->max_players) {
            world->status = 2; // full
        } else {
            world->status = 1; // online
        }
        
        /* Both fields are now as wide as their source; see WorldInfo. */
        strncpy(world->host, ws->host, sizeof(world->host) - 1);
        world->host[sizeof(world->host) - 1] = '\0';
        
        world->port = htons(ws->port);
        
        /* From the world table, not the literal "Unknown" this used to send:
         * the realm parsed a region out of its config and then threw it away
         * because WorldServer had nowhere to keep it. */
        const char* region = ws->region[0] ? ws->region : "Unknown";
        strncpy(world->region, region, sizeof(world->region) - 1);
        world->region[sizeof(world->region) - 1] = '\0';
    }
    
    int world_count = listed;
    pthread_mutex_unlock(&g_server.world_servers_lock);

    size_t send_size = offsetof(WorldListResponsePacket, worlds) +
                       (size_t)response.count * sizeof(WorldInfo);
    response.header.payload_size = htons((uint16_t)(send_size - sizeof(PacketHeader)));
    tls_send(client_fd, &response, send_size, 0);
    LOG_INFO("Sent world list with %d worlds to account %u", world_count, account_id);
}

/**
 * Validate character ownership and issue a short-lived ticket for an online world.
 *
 * The world-server mutex remains held while the ticket is stored in Redis.
 */
void world_enter(int client_fd, uint32_t account_id, uint8_t* buffer, ssize_t bytes) {
    if (bytes < (ssize_t)sizeof(EnterWorldPacket)) {
        LOG_ERROR("Invalid enter world packet size");
        return;
    }
    
    EnterWorldPacket* pkt = (EnterWorldPacket*)buffer;
    uint32_t character_id = ntohl(pkt->character_id);
    uint32_t requested_world_id = ntohl(pkt->world_id);
    
    LOG_INFO("Character %u wants to enter world %u", character_id, requested_world_id);
    
    // SECURITY: Verify character belongs to account
    if (!realm_character_owned(character_id, account_id, requested_world_id)) {
        EnterWorldResponsePacket response = {0};
        response.header.type = PACKET_ENTER_WORLD_RESPONSE;
        response.header.player_id = htonl(account_id);
        response.header.payload_size = htons(sizeof(response) - sizeof(PacketHeader));
        response.success = 0;
        strncpy(response.message, "Character does not belong to account", sizeof(response.message) - 1);
        response.message[sizeof(response.message) - 1] = '\0';
        tls_send(client_fd, &response, sizeof(response), 0);
        LOG_WARN("SECURITY: Account %u tried to access character %u (not owned)", 
                 account_id, character_id);
        return;
    }
    
    // Copy out what we need and release the lock immediately. Everything that
    // follows -- generating a ticket, a Redis round-trip, and the socket send --
    // used to run with world_servers_lock held, which blocked every other
    // client's world list or world entry behind this one player's Redis latency.
    int      world_found  = 0;
    int      world_online = 0;
    int      world_full   = 0;
    char     world_name[64];
    char     world_host[64];
    uint16_t world_port   = 0;

    world_name[0] = '\0';
    world_host[0] = '\0';

    int world_index = requested_world_id - 1;  // world_id is 1-indexed

    pthread_mutex_lock(&g_server.world_servers_lock);
    if (world_index >= 0 && world_index < g_server.num_world_servers) {
        const WorldServer* ws = &g_server.world_servers[world_index];
        world_found  = 1;
        world_online = ws->online;
        world_port   = ws->port;
        /* The same test world_send_list publishes as status 2, so the two
         * answers about one world agree. It was computed for the list and
         * nowhere else, so the realm minted tickets for worlds it had just
         * told the client were full -- the world then refused them at
         * admission, which is a round trip and a confusing error message to
         * arrive at a conclusion the realm already held. Capacity of 0 means
         * "not reported yet", not "no room"; see load_world_servers_from_table. */
        world_full = (ws->max_players > 0 && ws->player_count >= ws->max_players);
        snprintf(world_name, sizeof(world_name), "%s", ws->name);
        snprintf(world_host, sizeof(world_host), "%s", ws->host);
    }
    pthread_mutex_unlock(&g_server.world_servers_lock);
    
    EnterWorldResponsePacket response;
    memset(&response, 0, sizeof(response));
    response.header.type = PACKET_ENTER_WORLD_RESPONSE;
    response.header.player_id = htonl(account_id);
    response.header.payload_size = htons(sizeof(response) - sizeof(PacketHeader));
    
    if (!world_found) {
        response.success = 0;
        strncpy(response.message, "World not found", sizeof(response.message) - 1);
        response.message[sizeof(response.message) - 1] = '\0';
        tls_send(client_fd, &response, sizeof(response), 0);
        LOG_ERROR("Error: World %u not found", requested_world_id);
        return;
    }
    
    if (!world_online) {
        response.success = 0;
        strncpy(response.message, "World is offline", sizeof(response.message) - 1);
        response.message[sizeof(response.message) - 1] = '\0';
        tls_send(client_fd, &response, sizeof(response), 0);
        LOG_ERROR("Error: World %u is offline", requested_world_id);
        return;
    }

    if (world_full) {
        response.success = 0;
        strncpy(response.message, "That world is full. Please choose another.",
                sizeof(response.message) - 1);
        response.message[sizeof(response.message) - 1] = '\0';
        tls_send(client_fd, &response, sizeof(response), 0);
        LOG_INFO("Refused world entry for account %u: world %u is full",
                 account_id, requested_world_id);
        return;
    }
    
	// The address this ticket is issued to travels with it, so the world can
	// refuse a ticket redeemed from anywhere else.
	char client_ip[PEER_ADDR_MAXLEN] = {0};
	peer_addr_text(client_fd, client_ip, sizeof(client_ip));

	char game_ticket[REALM_TICKET_SIZE];
	realm_ticket_result issued = realm_issue_world_ticket(account_id, character_id,
	                                                      requested_world_id,
	                                                      client_ip, game_ticket);

	if(issued != REALM_TICKET_OK) {
		response.success = 0;
		strncpy(response.message, "Failed to generate ticket", sizeof(response.message) - 1);
		response.message[sizeof(response.message) - 1] = '\0';
		tls_send(client_fd, &response, sizeof(response), 0);

		if(issued == REALM_TICKET_NO_ENTROPY)
			LOG_ERROR("Error: no entropy available for a world-entry ticket");
		else if(issued == REALM_TICKET_NO_PEER)
			LOG_ERROR("Error: could not resolve the peer address for a world ticket");
		else
			LOG_ERROR("Error: Failed to store game ticket in Redis");
		return;
	}

    response.success = 1;
    strncpy(response.game_ticket, game_ticket, sizeof(response.game_ticket) - 1);
    response.game_ticket[sizeof(response.game_ticket) - 1] = '\0';
    strncpy(response.world_host, world_host, sizeof(response.world_host) - 1);
    response.world_host[sizeof(response.world_host) - 1] = '\0';
    response.world_port = htons(world_port);
    snprintf(response.message, sizeof(response.message), "Connecting to %s...", world_name);
    response.message[sizeof(response.message) - 1] = '\0';
    
    tls_send(client_fd, &response, sizeof(response), 0);
    LOG_INFO("Sent game ticket for world '%s' (%s:%d) to account %u", 
             world_name, world_host, world_port, account_id);
}
