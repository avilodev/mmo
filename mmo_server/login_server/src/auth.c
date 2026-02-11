#include "auth.h"
#include "users_database.h"
#include "session.h"
#include "utils.h"

#include <stdio.h>
#include <string.h> 
#include <arpa/inet.h>

void auth_handle_login(int client_fd, AuthLoginPacket* packet) {
    printf("[STAGE 1] Login validation: username=%s\n", packet->username);
    
    // Verify credentials
    uint32_t db_player_id = db_verify_user(packet->username, packet->password);
    printf("DEBUG: db_verify_user returned: %u\n", db_player_id); 
    
    AuthLoginResponsePacket response;
    memset(&response, 0, sizeof(response));
    response.header.type = PACKET_AUTH_RESPONSE;
    response.header.player_id = htonl(db_player_id);
    response.header.payload_size = htons(sizeof(response) - sizeof(PacketHeader));
    
    if (db_player_id > 0) {
        // SUCCESS: Return player ID, but don't create session yet
        response.success = 1;
        response.player_id = htonl(db_player_id);
        strncpy(response.message, "Credentials validated", 127);
        
        printf("[STAGE 1] SUCCESS: user=%s, player_id=%u (no session created yet)\n", 
               packet->username, db_player_id);
    } else {
        // FAILURE: Invalid credentials
        response.success = 0;
        response.player_id = 0;
        strncpy(response.message, "Invalid credentials", 127);
        
        printf("[STAGE 1] FAILED: invalid credentials for user=%s\n", packet->username);
    }
    
    printf("[DEBUG] Response packet bytes:\n");
    uint8_t* bytes = (uint8_t*)&response;
    for (int i = 0; i < 20 && i < sizeof(response); i++) {
        printf("%02x ", bytes[i]);
        if ((i + 1) % 16 == 0) printf("\n");
    }
    printf("\n");
    
    ssize_t sent = send(client_fd, &response, sizeof(response), 0);
    printf("[STAGE 1] Sent validation response: %zd bytes\n", sent);
}


void auth_handle_start_game(int client_fd, StartGameRequestPacket* packet) {
    uint32_t player_id = ntohl(packet->player_id);
    
    printf("[STAGE 2] Start game request: player_id=%u, username=%s\n", 
           player_id, packet->username);
    
    StartGameResponsePacket response;
    memset(&response, 0, sizeof(response));
    response.header.type = PACKET_START_GAME_RESPONSE;
    response.header.player_id = htonl(player_id);
    
    // Create session in Redis
    char session_key[SESSION_KEY_LENGTH];
    
    if (session_create(player_id, "127.0.0.1", session_key)) {
        // SUCCESS: Return session key
        response.success = 1;
        memcpy(response.header.session_key, session_key, 32);
        strncpy(response.message, "Session created - launching game", 127);
        
        printf("[STAGE 2] SUCCESS: player_id=%u, session_key=", player_id);
        for (int i = 0; i < 32; i++) {
            printf("%c", session_key[i]);
        }
        printf("\n");
        
        ssize_t sent = send(client_fd, &response, sizeof(response), 0);
        printf("[STAGE 2] Sent session response: %zd bytes\n", sent);
    } else {
        // FAILURE: Session creation failed
        response.success = 0;
        strncpy(response.message, "Failed to create session", 127);
        
        printf("[STAGE 2] FAILED: session creation error for player_id=%u\n", player_id);
        
        send(client_fd, &response, sizeof(response), 0);
    }
} 

void auth_handle_register(int client_fd, AuthRegisterPacket* packet) {
    printf("[REGISTER] Username: '%s', Email: '%s', Birthday: '%s'\n",
           packet->username, packet->email, packet->birthday);
    
    AuthRegisterResponsePacket response;
    memset(&response, 0, sizeof(response));
    response.header.type = PACKET_AUTH_RESPONSE;
    
    // Validate username
    if (!validate_username(packet->username)) {
        response.success = 0;
        strncpy(response.message, "Username is required", 127);
        send(client_fd, &response, sizeof(response), 0);
        return;
    }
    
    // Validate password
    if (!validate_password(packet->password)) {
        response.success = 0;
        strncpy(response.message, "Password must be at least 6 characters", 127);
        send(client_fd, &response, sizeof(response), 0);
        return;
    }
    
    // Validate email
    if (!validate_email(packet->email)) {
        response.success = 0;
        strncpy(response.message, "Invalid email format", 127);
        send(client_fd, &response, sizeof(response), 0);
        return;
    }
    
    // Validate birthday
    if (!validate_birthday(packet->birthday)) {
        response.success = 0;
        strncpy(response.message, "Invalid birthday format (use YYYY-MM-DD)", 127);
        send(client_fd, &response, sizeof(response), 0);
        return;
    }
    
    // Create user
    uint32_t new_player_id = db_create_user(packet->username, packet->password,
                                             packet->email, packet->birthday);
    
    if (new_player_id > 0) {
        response.success = 1;
        response.header.player_id = htonl(new_player_id);  // Set in header
        response.player_id = htonl(new_player_id);         // Set in body
        strncpy(response.message, "Registration successful! You can now log in.", 127);
        printf("[REGISTER] SUCCESS: player_id=%u\n", new_player_id);
    } else {
        response.success = 0;
        strncpy(response.message, "Username already exists", 127);
        printf("[REGISTER] FAILED: username exists\n");
    }
    
    send(client_fd, &response, sizeof(response), 0);
}