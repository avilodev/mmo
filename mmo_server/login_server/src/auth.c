#include "auth.h"
#include "rate_limiter.h"
#include "users_database.h"
#include "session.h"
#include "utils.h"
#include "tls.h"

#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <arpa/inet.h>

void auth_handle_login(int client_fd, AuthLoginPacket* packet) {
    printf("[STAGE 1] Login validation: username=%s\n", packet->username);

    char peer_ip[RL_IP_MAXLEN] = {0};
    rate_limiter_peer_ip(client_fd, peer_ip, sizeof(peer_ip));

    // Re-check the block here, not just at accept(). Credential verification
    // hashes the password, so a blocked address must be turned away before it
    // can spend that CPU.
    if (rate_limiter_check(peer_ip)) {
        AuthLoginResponsePacket blocked;
        memset(&blocked, 0, sizeof(blocked));
        blocked.header.type         = PACKET_AUTH_RESPONSE;
        blocked.header.payload_size = htons(sizeof(blocked) - sizeof(PacketHeader));
        blocked.success             = 0;
        strncpy(blocked.message, "Too many failed attempts. Try again later.", 127);
        tls_send(client_fd, &blocked, sizeof(blocked), 0);
        printf("[STAGE 1] REJECTED: blocked address %s\n", peer_ip);
        return;
    }

    // Verify credentials
    uint32_t db_player_id = db_verify_user(packet->username, packet->password);
    printf("DEBUG: db_verify_user returned: %u\n", db_player_id); 
    
    AuthLoginResponsePacket response;
    memset(&response, 0, sizeof(response));
    response.header.type = PACKET_AUTH_RESPONSE;
    response.header.player_id = htonl(db_player_id);
    response.header.payload_size = htons(sizeof(response) - sizeof(PacketHeader));
    
    if (db_player_id > 0) {
        char* auth_token = generate_session_key();
        if (!auth_token || !auth_token_store(auth_token, db_player_id)) {
            response.success = 0;
            response.header.player_id = 0;
            response.player_id = 0;
            strncpy(response.message, "Authentication service unavailable", 127);
            free(auth_token);
        } else {
            // The token, not the client-provided player ID, authorizes stage 2.
            response.success = 1;
            response.player_id = htonl(db_player_id);
            memcpy(response.auth_token, auth_token, sizeof(response.auth_token));
            strncpy(response.message, "Credentials validated", 127);
            free(auth_token);
        }
        
        if (response.success) {
            // Clear accumulated failures so an honest user who mistyped a few
            // times is not left one slip away from a block.
            rate_limiter_note_success(peer_ip);
            printf("[STAGE 1] SUCCESS: user=%s, player_id=%u (one-time token issued)\n",
                   packet->username, db_player_id);
        } else {
            printf("[STAGE 1] FAILED: could not issue login token for player_id=%u\n",
                   db_player_id);
        }
    } else {
        // FAILURE: Invalid credentials
        response.success = 0;
        response.player_id = 0;
        strncpy(response.message, "Invalid credentials", 127);

        if (rate_limiter_record_failure(peer_ip))
            strncpy(response.message, "Too many failed attempts. Try again later.", 127);

        printf("[STAGE 1] FAILED: invalid credentials for user=%s (ip=%s)\n",
               packet->username, peer_ip);
    }
    
    ssize_t sent = tls_send(client_fd, &response, sizeof(response), 0);
    if (sent != (ssize_t)sizeof(response))
        printf("[STAGE 1] Warning: partial/failed send (%zd/%zu bytes)\n",
               sent, sizeof(response));
    else
        printf("[STAGE 1] Sent validation response: %zd bytes\n", sent);
}


void auth_handle_start_game(int client_fd, StartGameRequestPacket* packet) {
    // Consume the single-use stage-1 proof and derive identity from Redis.
    // Never authorize using the player ID supplied by the client.
    uint32_t player_id = auth_token_consume(packet->auth_token);
    
    printf("[STAGE 2] Start game request for validated player_id=%u\n", player_id);
    
    StartGameResponsePacket response;
    memset(&response, 0, sizeof(response));
    response.header.type = PACKET_START_GAME_RESPONSE;
    response.header.player_id = htonl(player_id);
    response.header.payload_size = htons(sizeof(StartGameResponsePacket) - sizeof(PacketHeader));
    
    if (player_id == 0) {
        response.success = 0;
        strncpy(response.message, "Invalid or expired login token", 127);
        tls_send(client_fd, &response, sizeof(response), 0);
        return;
    }

    char peer_ip[RL_IP_MAXLEN] = {0};
    rate_limiter_peer_ip(client_fd, peer_ip, sizeof(peer_ip));

    // Create session in Redis
    char session_key[SESSION_KEY_LENGTH];
    
    if (session_create(player_id, peer_ip[0] ? peer_ip : "unknown", session_key)) {
        // SUCCESS: Return session key
        response.success = 1;
        memcpy(response.header.session_key, session_key, 32);
        strncpy(response.message, "Session created - launching game", 127);
        
        printf("[STAGE 2] SUCCESS: session created for player_id=%u\n", player_id);
        
        ssize_t sent = tls_send(client_fd, &response, sizeof(response), 0);
        if (sent != (ssize_t)sizeof(response))
            printf("[STAGE 2] Warning: partial/failed send (%zd/%zu bytes)\n",
                   sent, sizeof(response));
        else
            printf("[STAGE 2] Sent session response: %zd bytes\n", sent);
    } else {
        // FAILURE: Session creation failed
        response.success = 0;
        strncpy(response.message, "Failed to create session", 127);

        printf("[STAGE 2] FAILED: session creation error for player_id=%u\n", player_id);

        ssize_t sent = tls_send(client_fd, &response, sizeof(response), 0);
        if (sent != (ssize_t)sizeof(response))
            printf("[STAGE 2] Warning: partial/failed send on failure response\n");
    }
} 

void auth_handle_register(int client_fd, AuthRegisterPacket* packet) {
    printf("[REGISTER] Username: '%s', Email: '%s', Birthday: '%s'\n",
           packet->username, packet->email, packet->birthday);
    
    AuthRegisterResponsePacket response;
    memset(&response, 0, sizeof(response));
    response.header.type = PACKET_AUTH_RESPONSE;
    response.header.payload_size = htons(sizeof(response) - sizeof(PacketHeader));

    {
        char peer_ip[RL_IP_MAXLEN] = {0};
        rate_limiter_peer_ip(client_fd, peer_ip, sizeof(peer_ip));
        if (rate_limiter_check(peer_ip)) {
            response.success = 0;
            strncpy(response.message, "Too many failed attempts. Try again later.", 127);
            tls_send(client_fd, &response, sizeof(response), 0);
            printf("[REGISTER] REJECTED: blocked address %s\n", peer_ip);
            return;
        }
    }

    // Validate username
    if (!validate_username(packet->username)) {
        response.success = 0;
        strncpy(response.message, "Username is required", 127);
        tls_send(client_fd, &response, sizeof(response), 0);
        return;
    }
    
    // Validate password
    if (!validate_password(packet->password)) {
        response.success = 0;
        strncpy(response.message, "Password must be at least 6 characters", 127);
        tls_send(client_fd, &response, sizeof(response), 0);
        return;
    }
    
    // Validate email
    if (!validate_email(packet->email)) {
        response.success = 0;
        strncpy(response.message, "Invalid email format", 127);
        tls_send(client_fd, &response, sizeof(response), 0);
        return;
    }
    
    // Validate birthday
    if (!validate_birthday(packet->birthday)) {
        response.success = 0;
        strncpy(response.message, "Invalid birthday format (use YYYY-MM-DD)", 127);
        tls_send(client_fd, &response, sizeof(response), 0);
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

        char peer_ip[RL_IP_MAXLEN] = {0};
        rate_limiter_peer_ip(client_fd, peer_ip, sizeof(peer_ip));
        if (rate_limiter_record_failure(peer_ip))
            strncpy(response.message, "Too many failed attempts. Try again later.", 127);

        printf("[REGISTER] FAILED: username exists (ip=%s)\n", peer_ip);
    }
    
    tls_send(client_fd, &response, sizeof(response), 0);
}
