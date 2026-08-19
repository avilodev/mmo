/**
 * @file
 * Send realm world listings and issue single-use world-entry tickets.
 */
#include "world_list.h"

#include "world_database_manager.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <hiredis/hiredis.h>
#include <stddef.h>

/**
 * Generate a hexadecimal world-entry ticket from system random input.
 *
 * A timestamp-and-rand fallback is used when /dev/urandom cannot be opened.
 */
static void generate_secure_ticket(char* ticket_out, size_t size) {
    FILE* urandom = fopen("/dev/urandom", "r");
    if (urandom) {
        unsigned char random_bytes[24];
        fread(random_bytes, 1, 24, urandom);
        fclose(urandom);
        
        // Convert to hex string
        for (int i = 0; i < 24 && i*2 < (int)size-1; i++) {
            snprintf(ticket_out + i*2, size - i*2, "%02x", random_bytes[i]);
        }
    } else {
        // Fallback (less secure)
        snprintf(ticket_out, size, "TICKET_%lx_%lx", 
                (unsigned long)time(NULL), (unsigned long)rand());
    }
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
    
    response.count = g_server.num_world_servers;
    
    // Copy live world server data
    for (int i = 0; i < g_server.num_world_servers && i < MAX_WORLDS; i++) {
        WorldServer* ws = &g_server.world_servers[i];
        WorldInfo* world = &response.worlds[i];
        
        world->world_id = htonl(i + 1);
        
        // FIX: Properly handle all string fields
        strncpy(world->name, ws->name, sizeof(world->name) - 1);
        world->name[sizeof(world->name) - 1] = '\0';
        
        world->population = htons(ws->player_count);
        world->capacity = htons(ws->max_players);

        // Determine status based on player count vs max
        if (!ws->online) {
            world->status = 0; // offline
        } else if (ws->player_count >= ws->max_players) {
            world->status = 2; // full
        } else {
            world->status = 1; // online
        }
        
        strncpy(world->ip, ws->host, sizeof(world->ip) - 1);
        world->ip[sizeof(world->ip) - 1] = '\0';
        
        world->port = htons(ws->port);
        
        strncpy(world->region, "Unknown", sizeof(world->region) - 1);
        world->region[sizeof(world->region) - 1] = '\0';
    }
    
    int world_count = response.count;
    pthread_mutex_unlock(&g_server.world_servers_lock);

    size_t send_size = offsetof(WorldListResponsePacket, worlds) +
                       (size_t)response.count * sizeof(WorldInfo);
    response.header.payload_size = htons((uint16_t)(send_size - sizeof(PacketHeader)));
    send(client_fd, &response, send_size, 0);
    printf("Sent world list with %d worlds to account %u\n", world_count, account_id);
}

/**
 * Validate character ownership and issue a short-lived ticket for an online world.
 *
 * The world-server mutex remains held while the ticket is stored in Redis.
 */
void world_enter(int client_fd, uint32_t account_id, uint8_t* buffer, ssize_t bytes) {
    if (bytes < (ssize_t)sizeof(EnterWorldPacket)) {
        printf("Invalid enter world packet size\n");
        return;
    }
    
    EnterWorldPacket* pkt = (EnterWorldPacket*)buffer;
    uint32_t character_id = ntohl(pkt->character_id);
    uint32_t requested_world_id = ntohl(pkt->world_id);
    
    printf("Character %u wants to enter world %u\n", character_id, requested_world_id);
    
    // SECURITY: Verify character belongs to account
    if (!world_character_belongs_to_account(character_id, account_id, requested_world_id)) {
        EnterWorldResponsePacket response = {0};
        response.header.type = PACKET_ENTER_WORLD_RESPONSE;
        response.header.player_id = htonl(account_id);
        response.header.payload_size = htons(sizeof(response) - sizeof(PacketHeader));
        response.success = 0;
        strncpy(response.message, "Character does not belong to account", sizeof(response.message) - 1);
        response.message[sizeof(response.message) - 1] = '\0';
        send(client_fd, &response, sizeof(response), 0);
        printf("SECURITY: Account %u tried to access character %u (not owned)\n", 
               account_id, character_id);
        return;
    }
    
    // Copy out what we need and release the lock immediately. Everything that
    // follows -- generating a ticket, a Redis round-trip, and the socket send --
    // used to run with world_servers_lock held, which blocked every other
    // client's world list or world entry behind this one player's Redis latency.
    int      world_found  = 0;
    int      world_online = 0;
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
        send(client_fd, &response, sizeof(response), 0);
        printf("Error: World %u not found\n", requested_world_id);
        return;
    }
    
    if (!world_online) {
        response.success = 0;
        strncpy(response.message, "World is offline", sizeof(response.message) - 1);
        response.message[sizeof(response.message) - 1] = '\0';
        send(client_fd, &response, sizeof(response), 0);
        printf("Error: World %u is offline\n", requested_world_id);
        return;
    }
    
    // Generate cryptographically secure game ticket
    char game_ticket[65];
    generate_secure_ticket(game_ticket, sizeof(game_ticket));
    
    // Store ticket in Redis with 60 second expiration
    // Format: ticket:XXXXX -> "character_id:world_id:account_id"
    char ticket_key[128];
    snprintf(ticket_key, sizeof(ticket_key), "ticket:%s", game_ticket);
    
    char ticket_value[128];
    snprintf(ticket_value, sizeof(ticket_value), "%u:%u:%u", 
             character_id, requested_world_id, account_id);
    
    if (!store_game_ticket_in_redis(ticket_key, ticket_value, 60)) {
        response.success = 0;
        strncpy(response.message, "Failed to generate ticket", sizeof(response.message) - 1);
        response.message[sizeof(response.message) - 1] = '\0';
        send(client_fd, &response, sizeof(response), 0);
        printf("Error: Failed to store game ticket in Redis\n");
        return;
    }
    
    response.success = 1;
    strncpy(response.game_ticket, game_ticket, sizeof(response.game_ticket) - 1);
    response.game_ticket[sizeof(response.game_ticket) - 1] = '\0';
    strncpy(response.world_ip, world_host, sizeof(response.world_ip) - 1);
    response.world_ip[sizeof(response.world_ip) - 1] = '\0';
    response.world_port = htons(world_port);
    snprintf(response.message, sizeof(response.message), "Connecting to %s...", world_name);
    response.message[sizeof(response.message) - 1] = '\0';
    
    send(client_fd, &response, sizeof(response), 0);
    printf("Sent game ticket for world '%s' (%s:%d) to account %u\n", 
           world_name, world_host, world_port, account_id);
}
