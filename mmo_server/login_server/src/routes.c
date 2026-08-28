/**
 * @file
 * Validate and dispatch login-server packet types to their handlers.
 */
#include "routes.h"
#include "log.h"
#include "auth.h"
#include "patch_notes.h"

#include <stdio.h>
#include <arpa/inet.h>

/**
 * Route one complete login packet according to its wire type.
 *
 * Credential text fields in mutable packet_data are forced to terminate before dispatch.
 */
void route_packet(int client_fd, void* packet_data, ssize_t bytes) {
    if (bytes < MIN_HEADER_SIZE) {
        /* Client-controlled and reached before any budget is spent. */
        LOG_WARN_RL(10, 60, "Packet too small: %zd bytes", bytes);
        return;
    }
    
    PacketHeader* header = (PacketHeader*)packet_data;
    
    /* Debug, not info: one line per packet, on the path every login walks.
     * Each handler below logs its own outcome; this is the trace you turn on
     * when that outcome does not explain itself. */
    LOG_DEBUG("Routing packet type: %d", header->type);
    
    switch (header->type) {
        case PACKET_AUTH_LOGIN:
            LOG_DEBUG("-> Handling AUTH_LOGIN (validation only)");
            if (bytes >= (ssize_t)sizeof(AuthLoginPacket)) {
                AuthLoginPacket* packet = (AuthLoginPacket*)packet_data;
                packet->username[sizeof(packet->username) - 1] = '\0';
                packet->password[sizeof(packet->password) - 1] = '\0';
                auth_handle_login(client_fd, packet);
            } else {
                LOG_ERROR_RL(10, 60, "Invalid login packet size: %zd", bytes);
            }
            break;
            
        case PACKET_START_GAME_REQUEST:
            LOG_DEBUG("-> Handling START_GAME_REQUEST (session creation)");
            if (bytes >= (ssize_t)sizeof(StartGameRequestPacket)) {
                auth_handle_start_game(client_fd, (StartGameRequestPacket*)packet_data);
            } else {
                LOG_ERROR_RL(10, 60, "Invalid start game packet size: %zd (expected %zu)",
                             bytes, sizeof(StartGameRequestPacket));
            }
            break;
            
        case PACKET_AUTH_REGISTER:
            LOG_DEBUG("-> Handling AUTH_REGISTER");
            if (bytes >= AUTH_REGISTER_SIZE) { 
                AuthRegisterPacket* packet = (AuthRegisterPacket*)packet_data;
                packet->username[sizeof(packet->username) - 1] = '\0';
                packet->password[sizeof(packet->password) - 1] = '\0';
                packet->email[sizeof(packet->email) - 1] = '\0';
                packet->birthday[sizeof(packet->birthday) - 1] = '\0';
                auth_handle_register(client_fd, packet);
            } else {
                LOG_ERROR_RL(10, 60, "Invalid register packet size: %zd (expected 215)", bytes);
            }
            break;

        case PATCH_NOTES_REQUEST:
            LOG_DEBUG("-> Handling Patch Notes Request");
            if (bytes >= MIN_HEADER_SIZE) {
                handle_patch_notes_request(client_fd, packet_data, bytes);
            } else {
                LOG_ERROR_RL(10, 60, "Invalid Patch Notes packet size: %zd", bytes);
            }
            break;
             
        default:
            LOG_WARN_RL(10, 60, "Unknown packet type: %d", header->type);
            break;
    }
}
