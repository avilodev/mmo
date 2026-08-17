/**
 * @file
 * Validate and dispatch login-server packet types to their handlers.
 */
#include "routes.h"
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
        printf("Packet too small: %zd bytes\n", bytes);
        return;
    }
    
    PacketHeader* header = (PacketHeader*)packet_data;
    
    printf("Routing packet type: %d\n", header->type);
    
    switch (header->type) {
        case PACKET_AUTH_LOGIN:
            printf("-> Handling AUTH_LOGIN (validation only)\n");
            if (bytes >= (ssize_t)sizeof(AuthLoginPacket)) {
                AuthLoginPacket* packet = (AuthLoginPacket*)packet_data;
                packet->username[sizeof(packet->username) - 1] = '\0';
                packet->password[sizeof(packet->password) - 1] = '\0';
                auth_handle_login(client_fd, packet);
            } else {
                printf("Invalid login packet size: %zd\n", bytes);
            }
            break;
            
        case PACKET_START_GAME_REQUEST:
            printf("-> Handling START_GAME_REQUEST (session creation)\n");
            if (bytes >= (ssize_t)sizeof(StartGameRequestPacket)) {
                auth_handle_start_game(client_fd, (StartGameRequestPacket*)packet_data);
            } else {
                printf("Invalid start game packet size: %zd (expected %zu)\n", 
                       bytes, sizeof(StartGameRequestPacket));
            }
            break;
            
        case PACKET_AUTH_REGISTER:
            printf("-> Handling AUTH_REGISTER\n");
            if (bytes >= AUTH_REGISTER_SIZE) { 
                AuthRegisterPacket* packet = (AuthRegisterPacket*)packet_data;
                packet->username[sizeof(packet->username) - 1] = '\0';
                packet->password[sizeof(packet->password) - 1] = '\0';
                packet->email[sizeof(packet->email) - 1] = '\0';
                packet->birthday[sizeof(packet->birthday) - 1] = '\0';
                auth_handle_register(client_fd, packet);
            } else {
                printf("Invalid register packet size: %zd (expected 215)\n", bytes);
            }
            break;

        case PATCH_NOTES_REQUEST:
            printf("-> Handling Patch Notes Request\n");
            if (bytes >= MIN_HEADER_SIZE) {
                handle_patch_notes_request(client_fd, packet_data, bytes);
            } else {
                printf("Invalid Patch Notes packet size: %zd\n", bytes);
            }
            break;
             
        default:
            printf("Unknown packet type: %d\n", header->type);
            break;
    }
}
