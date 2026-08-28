/**
 * @file
 * Validate login and registration requests and issue staged authentication credentials.
 */
#include "auth.h"
#include "log.h"
#include "rate_limiter.h"
#include "users_database.h"
#include "session.h"
#include "utils.h"
#include "tls.h"

#include <stdio.h>
#include <string.h>
#include <sys/socket.h> 
#include <arpa/inet.h>

/**
 * Validate credentials and return a short-lived single-use authentication token.
 *
 * The response derives its player identifier from the account database and encodes protocol integers in network byte order.
 */
void auth_handle_login(int client_fd, AuthLoginPacket* packet) {
    /* The username is an account identifier a person chose, and it arrives
     * here on every attempt including the failed ones -- which is to say the
     * login log was a list of names to try. It stays available at LOG_DEBUG
     * for the case where someone is deliberately following one account
     * through a problem; what the operational log keeps is the address, which
     * is what a flood is diagnosed from. The same rule applies to the email
     * and birthday in registration below. */
    LOG_DEBUG("[STAGE 1] Login validation: username=%s", packet->username);

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
        LOG_INFO("[STAGE 1] REJECTED: blocked address %s", peer_ip);
        return;
    }

    // Verify credentials
    uint32_t db_player_id = db_verify_user(packet->username, packet->password);
    LOG_DEBUG("[STAGE 1] db_verify_user returned player_id=%u", db_player_id);
    
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
            LOG_INFO("[STAGE 1] SUCCESS: player_id=%u (one-time token issued)", db_player_id);
            LOG_DEBUG("[STAGE 1] SUCCESS was for user=%s", packet->username);
        } else {
            LOG_INFO("[STAGE 1] FAILED: could not issue login token for player_id=%u",
                     db_player_id);
        }
    } else {
        // FAILURE: Invalid credentials
        response.success = 0;
        response.player_id = 0;
        strncpy(response.message, "Invalid credentials", 127);

        if (rate_limiter_record_failure(peer_ip))
            strncpy(response.message, "Too many failed attempts. Try again later.", 127);

        LOG_INFO("[STAGE 1] FAILED: invalid credentials (ip=%s)", peer_ip);
        LOG_DEBUG("[STAGE 1] FAILED attempt was for user=%s", packet->username);
    }
    
    ssize_t sent = tls_send(client_fd, &response, sizeof(response), 0);
    if (sent != (ssize_t)sizeof(response))
        LOG_INFO("[STAGE 1] Warning: partial/failed send (%zd/%zu bytes)",
                 sent, sizeof(response));
    else
        LOG_DEBUG("[STAGE 1] Sent validation response: %zd bytes", sent);
}


/**
 * Consume a staged authentication token and return a new world-session key.
 *
 * Client-supplied identity fields do not authorize the request; identity comes from the consumed Redis token.
 */
void auth_handle_start_game(int client_fd, StartGameRequestPacket* packet) {
    // Consume the single-use stage-1 proof and derive identity from Redis.
    // Never authorize using the player ID supplied by the client.
    uint32_t player_id = auth_token_consume(packet->auth_token);
    
    LOG_INFO("[STAGE 2] Start game request for validated player_id=%u", player_id);
    
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
        
        LOG_INFO("[STAGE 2] SUCCESS: session created for player_id=%u", player_id);
        
        ssize_t sent = tls_send(client_fd, &response, sizeof(response), 0);
        if (sent != (ssize_t)sizeof(response))
            LOG_INFO("[STAGE 2] Warning: partial/failed send (%zd/%zu bytes)",
                     sent, sizeof(response));
        else
            LOG_DEBUG("[STAGE 2] Sent session response: %zd bytes", sent);
    } else {
        // FAILURE: Session creation failed
        response.success = 0;
        strncpy(response.message, "Failed to create session", 127);

        LOG_INFO("[STAGE 2] FAILED: session creation error for player_id=%u", player_id);

        ssize_t sent = tls_send(client_fd, &response, sizeof(response), 0);
        if (sent != (ssize_t)sizeof(response))
            LOG_INFO("[STAGE 2] Warning: partial/failed send on failure response");
    }
} 

/**
 * Validate registration fields, create an account, and send the resulting player identifier.
 *
 * Response integer fields are encoded in network byte order.
 */
void auth_handle_register(int client_fd, AuthRegisterPacket* packet) {
    LOG_DEBUG("[REGISTER] Username: '%s', Email: '%s', Birthday: '%s'",
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
            LOG_INFO("[REGISTER] REJECTED: blocked address %s", peer_ip);
            return;
        }

        /* Charged before any validation runs, so a malformed attempt costs the
         * same as a well-formed one and probing is not free. Everything below
         * this point either creates an account or spends Argon2id deciding not
         * to; the failure counter never saw either, because a registration
         * with an unused username always succeeded. */
        if (rate_limiter_record_registration(peer_ip)) {
            response.success = 0;
            strncpy(response.message,
                    "Too many accounts created from this address. Try again later.", 127);
            tls_send(client_fd, &response, sizeof(response), 0);
            LOG_WARN("[REGISTER] REJECTED: registration budget exhausted for %s", peer_ip);
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
        LOG_INFO("[REGISTER] SUCCESS: player_id=%u", new_player_id);
    } else {
        response.success = 0;
        strncpy(response.message, "Username already exists", 127);

        char peer_ip[RL_IP_MAXLEN] = {0};
        rate_limiter_peer_ip(client_fd, peer_ip, sizeof(peer_ip));
        if (rate_limiter_record_failure(peer_ip))
            strncpy(response.message, "Too many failed attempts. Try again later.", 127);

        LOG_INFO("[REGISTER] FAILED: username exists (ip=%s)", peer_ip);
    }
    
    tls_send(client_fd, &response, sizeof(response), 0);
}
