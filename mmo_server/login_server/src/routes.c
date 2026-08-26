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
        LOG_INFO("Packet too small: %zd bytes", bytes);
        return;
    }
    
    PacketHeader* header = (PacketHeader*)packet_data;
    
    LOG_INFO("Routing packet type: %d", header->type);
    
    switch (header->type) {
        case PACKET_AUTH_LOGIN:
            LOG_INFO("-> Handling AUTH_LOGIN (validation only)");
            if (bytes >= (ssize_t)sizeof(AuthLoginPacket)) {
                AuthLoginPacket* packet = (AuthLoginPacket*)packet_data;
                packet->username[sizeof(packet->username) - 1] = '\0';
                packet->password[sizeof(packet->password) - 1] = '\0';
                auth_handle_login(client_fd, packet);
            } else {
                LOG_ERROR("Invalid login packet size: %zd", bytes);
            }
            break;
            
        case PACKET_START_GAME_REQUEST:
            LOG_INFO("-> Handling START_GAME_REQUEST (session creation)");
            if (bytes >= (ssize_t)sizeof(StartGameRequestPacket)) {
                auth_handle_start_game(client_fd, (StartGameRequestPacket*)packet_data);
            } else {
                LOG_ERROR("Invalid start game packet size: %zd (expected %zu)", 
                         bytes, sizeof(StartGameRequestPacket));
            }
            break;
            
        case PACKET_AUTH_REGISTER:
            LOG_INFO("-> Handling AUTH_REGISTER");
            if (bytes >= AUTH_REGISTER_SIZE) { 
                AuthRegisterPacket* packet = (AuthRegisterPacket*)packet_data;
                packet->username[sizeof(packet->username) - 1] = '\0';
                packet->password[sizeof(packet->password) - 1] = '\0';
                packet->email[sizeof(packet->email) - 1] = '\0';
                packet->birthday[sizeof(packet->birthday) - 1] = '\0';
                auth_handle_register(client_fd, packet);
            } else {
                LOG_ERROR("Invalid register packet size: %zd (expected 215)", bytes);
            }
            break;

        case PATCH_NOTES_REQUEST:
            LOG_INFO("-> Handling Patch Notes Request");
            if (bytes >= MIN_HEADER_SIZE) {
                handle_patch_notes_request(client_fd, packet_data, bytes);
            } else {
                LOG_ERROR("Invalid Patch Notes packet size: %zd", bytes);
            }
            break;
             
        default:
            LOG_WARN("Unknown packet type: %d", header->type);
            break;
    }
}
