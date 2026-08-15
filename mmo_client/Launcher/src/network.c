#include "network.h"
#include "tls_client.h"
#include <stdio.h>
#include <string.h>

static BOOL g_networkInitialized = FALSE;

BOOL NetworkInit(void) {
    if (g_networkInitialized) return TRUE;

    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
        return FALSE;

    if (!tls_client_init()) {
        WSACleanup();
        return FALSE;
    }

    g_networkInitialized = TRUE;
    return TRUE;
}

void NetworkCleanup(void) {
    if (g_networkInitialized) {
        tls_client_cleanup();
        WSACleanup();
        g_networkInitialized = FALSE;
    }
}

// STAGE 1: Validate credentials (returns player_id if successful, no session created)
BOOL SendLoginRequest(const char* username, const char* password, uint32_t* out_player_id,
                      char out_auth_token[32], char* errorMsg, int errorMsgSize) {
    SOCKET sock = INVALID_SOCKET;
    struct sockaddr_in server_addr;
    AuthLoginPacket loginPacket;
    uint8_t response_buffer[512];  // Increased buffer size for safety
    
    // Create socket
    sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) {
        snprintf(errorMsg, errorMsgSize, "Failed to create socket: %d", WSAGetLastError());
        return FALSE;
    }
    
    // Set timeout (5 seconds)
    DWORD timeout = 5000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (char*)&timeout, sizeof(timeout));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (char*)&timeout, sizeof(timeout));
    
    // Setup server address
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons((u_short)g_login_server_port);
    
    if (inet_pton(AF_INET, g_login_server_ip, &server_addr.sin_addr) <= 0) {
        snprintf(errorMsg, errorMsgSize, "Invalid server IP address");
        closesocket(sock);
        return FALSE;
    }
    
    // Connect to server
    if (connect(sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        snprintf(errorMsg, errorMsgSize, "Connection failed: %d\nCannot Connect to Server", WSAGetLastError());
        closesocket(sock);
        return FALSE;
    }

    // TLS handshake
    SSL* ssl = tls_client_connect(sock);
    if (!ssl) {
        snprintf(errorMsg, errorMsgSize, "TLS handshake failed");
        closesocket(sock);
        return FALSE;
    }

    // Prepare login packet (just validation, no session)
    memset(&loginPacket, 0, sizeof(AuthLoginPacket));
    loginPacket.header.type = PACKET_AUTH_LOGIN;
    loginPacket.header.player_id = 0;
    loginPacket.header.payload_size = htons(sizeof(AuthLoginPacket) - sizeof(PacketHeader));
    
    strncpy(loginPacket.username, username, 31);
    loginPacket.username[31] = '\0';
    
    strncpy(loginPacket.password, password, 63);
    loginPacket.password[63] = '\0';
    
    printf("[LOGIN] Sending credentials for validation...\n");
    printf("[LOGIN] Packet size: %zu bytes\n", sizeof(AuthLoginPacket));
    
    // Send login request
    int sent = tls_client_send(ssl, (char*)&loginPacket, sizeof(AuthLoginPacket));
    if (sent != sizeof(AuthLoginPacket)) {
        snprintf(errorMsg, errorMsgSize, "Failed to send login packet (sent %d of %zu)", sent, sizeof(AuthLoginPacket));
        tls_client_close(ssl);
        closesocket(sock);
        return FALSE;
    }

    printf("[LOGIN] Sent %d bytes, waiting for response...\n", sent);

    // Receive response
    memset(response_buffer, 0, sizeof(response_buffer));
    int received = tls_client_recv_packet(ssl, (char*)response_buffer, sizeof(response_buffer));

    printf("[LOGIN] Received %d bytes from server\n", received);
    
    if (received <= 0) {
        snprintf(errorMsg, errorMsgSize, "No response from server (recv returned %d, WSA error: %d)", received, WSAGetLastError());
        tls_client_close(ssl);
        closesocket(sock);
        return FALSE;
    }

    if (received < 12) {  // Minimum: header(7) + success(1) + player_id(4) = 12 bytes
        snprintf(errorMsg, errorMsgSize, "Invalid response from server (got %d bytes, expected at least 12)", received);
        tls_client_close(ssl);
        closesocket(sock);
        return FALSE;
    }

    tls_client_close(ssl);
    closesocket(sock);
    
    // Parse response manually byte-by-byte to avoid struct packing issues
    int offset = 0;

    // PacketHeader: type(1) + player_id(4) + payload_size(2) = 7 bytes
    uint8_t response_type = response_buffer[offset++];
    uint32_t response_player_id_net;
    memcpy(&response_player_id_net, &response_buffer[offset], 4);
    offset += 4;
    uint32_t player_id = ntohl(response_player_id_net);  // THIS IS THE ACTUAL PLAYER ID!
    uint16_t response_payload_size_net;
    memcpy(&response_payload_size_net, &response_buffer[offset], 2);
    offset += 2;

    printf("[LOGIN] Parsed header: type=%d, player_id=%u, payload_size=%d\n", 
        response_type, player_id, ntohs(response_payload_size_net));

    // AuthLoginResponsePacket fields
    uint8_t success = response_buffer[offset++];
    uint32_t assigned_player_id_net;
    memcpy(&assigned_player_id_net, &response_buffer[offset], 4);
    offset += 4;
    // We don't actually need this duplicate player_id

    // Single-use proof required by the separate start-game connection.
    if (received < offset + 32) {
        snprintf(errorMsg, errorMsgSize, "Login response missing authentication token");
        return FALSE;
    }
    memcpy(out_auth_token, &response_buffer[offset], 32);
    offset += 32;

    // Message (rest of packet)
    char message[256] = {0};
    int message_len = received - offset;
    if (message_len < 0) message_len = 0;
    if (message_len > 255) message_len = 255;
    if (message_len > 0) {
        memcpy(message, &response_buffer[offset], message_len);
        message[message_len] = '\0';
    }

    printf("[LOGIN] Parsed response: type=%d, success=%d, player_id=%u, message='%s'\n", 
        response_type, success, player_id, message);

    // Check if login was successful
    if (success) {
        *out_player_id = player_id;  // Use the player_id from the HEADER
        printf("[LOGIN] SUCCESS! Player ID: %u (no session created yet)\n", player_id);
        
        if (message_len > 0) {
            strncpy(errorMsg, message, errorMsgSize - 1);
            errorMsg[errorMsgSize - 1] = '\0';
        } else {
            snprintf(errorMsg, errorMsgSize, "Login successful");
        }
        
        return TRUE;
    } else {
        printf("[LOGIN] FAILED: %s\n", message);
        
        if (message_len > 0) {
            strncpy(errorMsg, message, errorMsgSize - 1);
            errorMsg[errorMsgSize - 1] = '\0';
        } else {
            snprintf(errorMsg, errorMsgSize, "Invalid credentials");
        }
        
        return FALSE;
    }
}

// STAGE 2: Request game start (creates session and returns key)
BOOL SendStartGameRequest(uint32_t player_id, const char* username, const char auth_token[32],
                          char* out_session_key, char* errorMsg, int errorMsgSize) {
    SOCKET sock = INVALID_SOCKET;
    struct sockaddr_in server_addr;
    StartGameRequestPacket request;
    uint8_t response_buffer[512];  // Use buffer for flexible parsing
    
    // Create socket
    sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) {
        snprintf(errorMsg, errorMsgSize, "Failed to create socket: %d", WSAGetLastError());
        return FALSE;
    }
    
    // Set timeout (5 seconds)
    DWORD timeout = 5000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (char*)&timeout, sizeof(timeout));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (char*)&timeout, sizeof(timeout));
    
    // Setup server address
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons((u_short)g_login_server_port);
    
    if (inet_pton(AF_INET, g_login_server_ip, &server_addr.sin_addr) <= 0) {
        snprintf(errorMsg, errorMsgSize, "Invalid server IP address");
        closesocket(sock);
        return FALSE;
    }
    
    // Connect to server
    if (connect(sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        snprintf(errorMsg, errorMsgSize, "Connection failed: %d", WSAGetLastError());
        closesocket(sock);
        return FALSE;
    }

    // TLS handshake
    SSL* ssl = tls_client_connect(sock);
    if (!ssl) {
        snprintf(errorMsg, errorMsgSize, "TLS handshake failed");
        closesocket(sock);
        return FALSE;
    }

    // Prepare start game request
    memset(&request, 0, sizeof(StartGameRequestPacket));
    request.header.type = PACKET_START_GAME_REQUEST;
    request.header.player_id = htonl(player_id);
    request.header.payload_size = htons(sizeof(StartGameRequestPacket) - sizeof(PacketHeader));
    request.player_id = htonl(player_id);
    strncpy(request.username, username, 31);
    request.username[31] = '\0';
    memcpy(request.auth_token, auth_token, 32);
    
    printf("[START GAME] Requesting session creation for player %u...\n", player_id);
    
    // Send request
    int sent = tls_client_send(ssl, (char*)&request, sizeof(StartGameRequestPacket));
    if (sent != sizeof(StartGameRequestPacket)) {
        snprintf(errorMsg, errorMsgSize, "Failed to send start game request");
        tls_client_close(ssl);
        closesocket(sock);
        return FALSE;
    }

    // Receive response
    memset(response_buffer, 0, sizeof(response_buffer));
    int received = tls_client_recv_packet(ssl, (char*)response_buffer, sizeof(response_buffer));

    printf("[START GAME] Received %d bytes from server\n", received);

    if (received <= 0) {
        snprintf(errorMsg, errorMsgSize, "Failed to receive response from server");
        tls_client_close(ssl);
        closesocket(sock);
        return FALSE;
    }

    if (received < 40) {  // Minimum: AuthPacketHeader(39) + success(1) = 40 bytes
        snprintf(errorMsg, errorMsgSize, "Invalid response size (got %d bytes)", received);
        tls_client_close(ssl);
        closesocket(sock);
        return FALSE;
    }

    tls_client_close(ssl);
    closesocket(sock);
    
    // Parse response manually
    int offset = 0;
    
    // AuthPacketHeader: type(1) + player_id(4) + payload_size(2) + session_key(32) = 39 bytes
    offset++; // skip type
    offset += 4; // skip player_id
    offset += 2; // skip payload_size
    
    // Extract session key
    memcpy(out_session_key, &response_buffer[offset], 32);
    offset += 32;
    
    // Success flag
    uint8_t success = response_buffer[offset++];
    
    // Message (rest of packet)
    char message[256] = {0};
    int message_len = received - offset;
    if (message_len < 0) message_len = 0;
    if (message_len > 255) message_len = 255;
    if (message_len > 0) {
        memcpy(message, &response_buffer[offset], message_len);
        message[message_len] = '\0';
    }
    
    // Check if successful
    if (success) {
        printf("[START GAME] Session created successfully\n");
        
        if (message_len > 0) {
            strncpy(errorMsg, message, errorMsgSize - 1);
            errorMsg[errorMsgSize - 1] = '\0';
        } else {
            snprintf(errorMsg, errorMsgSize, "Session created successfully");
        }
        return TRUE;
    } else {
        printf("[START GAME] Failed: %s\n", message);
        if (message_len > 0) {
            strncpy(errorMsg, message, errorMsgSize - 1);
            errorMsg[errorMsgSize - 1] = '\0';
        } else {
            snprintf(errorMsg, errorMsgSize, "Failed to create session");
        }
        return FALSE;
    }
}

// Add this to network.c

// REGISTRATION: Create new account
BOOL SendRegisterRequest(const char* username, const char* password, const char* email, const char* birthday,
                         uint32_t* out_player_id, char* errorMsg, int errorMsgSize) {
    SOCKET sock = INVALID_SOCKET;
    struct sockaddr_in server_addr;
    uint8_t request_buffer[256];
    uint8_t response_buffer[512];
    
    // Create socket
    sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) {
        snprintf(errorMsg, errorMsgSize, "Failed to create socket: %d", WSAGetLastError());
        return FALSE;
    }
    
    // Set timeout (5 seconds)
    DWORD timeout = 5000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (char*)&timeout, sizeof(timeout));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (char*)&timeout, sizeof(timeout));
    
    // Setup server address
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons((u_short)g_login_server_port);
    
    if (inet_pton(AF_INET, g_login_server_ip, &server_addr.sin_addr) <= 0) {
        snprintf(errorMsg, errorMsgSize, "Invalid server IP address");
        closesocket(sock);
        return FALSE;
    }
    
    // Connect to server
    if (connect(sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        snprintf(errorMsg, errorMsgSize, "Connection failed: %d\nCannot Connect to Server", WSAGetLastError());
        closesocket(sock);
        return FALSE;
    }

    // TLS handshake
    SSL* ssl = tls_client_connect(sock);
    if (!ssl) {
        snprintf(errorMsg, errorMsgSize, "TLS handshake failed");
        closesocket(sock);
        return FALSE;
    }

    // Build registration packet manually
    int offset = 0;
    
    // PacketHeader: type(1) + player_id(4) + payload_size(2) = 7 bytes
    request_buffer[offset++] = PACKET_AUTH_REGISTER;
    
    uint32_t player_id_net = htonl(0);
    memcpy(&request_buffer[offset], &player_id_net, 4);
    offset += 4;
    
    // Payload size: username(32) + password(64) + email(64) + birthday(16) + reserved(32) = 208
    uint16_t payload_size_net = htons(208);
    memcpy(&request_buffer[offset], &payload_size_net, 2);
    offset += 2;
    
    // Username (32 bytes)
    char username_buf[32] = {0};
    strncpy(username_buf, username, 31);
    memcpy(&request_buffer[offset], username_buf, 32);
    offset += 32;
    
    // Password (64 bytes)
    char password_buf[64] = {0};
    strncpy(password_buf, password, 63);
    memcpy(&request_buffer[offset], password_buf, 64);
    offset += 64;
    
    // Email (64 bytes)
    char email_buf[64] = {0};
    if (email && strlen(email) > 0) {
        strncpy(email_buf, email, 63);
    }
    memcpy(&request_buffer[offset], email_buf, 64);
    offset += 64;
    
    // Birthday (16 bytes)
    char birthday_buf[16] = {0};
    if (birthday && strlen(birthday) > 0) {
        strncpy(birthday_buf, birthday, 15);
    }
    memcpy(&request_buffer[offset], birthday_buf, 16);
    offset += 16;
    
    // Reserved (32 bytes) - zeros
    char reserved[32] = {0};
    memcpy(&request_buffer[offset], reserved, 32);
    offset += 32;
    
    int packet_size = offset;  // Should be 215 bytes
    
    printf("[REGISTER] Sending registration for user: %s, email: %s, birthday: %s\n", 
           username, email ? email : "(none)", birthday ? birthday : "(none)");
    printf("[REGISTER] Packet size: %d bytes\n", packet_size);
    
    // Send registration request
    int sent = tls_client_send(ssl, (char*)request_buffer, packet_size);
    if (sent != packet_size) {
        snprintf(errorMsg, errorMsgSize, "Failed to send registration packet (sent %d of %d)", sent, packet_size);
        tls_client_close(ssl);
        closesocket(sock);
        return FALSE;
    }

    printf("[REGISTER] Sent %d bytes, waiting for response...\n", sent);

    // Receive response
    memset(response_buffer, 0, sizeof(response_buffer));
    int received = tls_client_recv_packet(ssl, (char*)response_buffer, sizeof(response_buffer));

    printf("[REGISTER] Received %d bytes from server\n", received);

    if (received <= 0) {
        snprintf(errorMsg, errorMsgSize, "No response from server (recv returned %d, WSA error: %d)",
                 received, WSAGetLastError());
        tls_client_close(ssl);
        closesocket(sock);
        return FALSE;
    }

    if (received < 12) {  // PacketHeader(7) + success(1) + player_id(4) = 12
        snprintf(errorMsg, errorMsgSize, "Invalid response from server (got %d bytes)", received);
        tls_client_close(ssl);
        closesocket(sock);
        return FALSE;
    }

    tls_client_close(ssl);
    closesocket(sock);

    // Parse response - AuthRegisterResponsePacket
    // Layout: PacketHeader(7) + success(1) + player_id(4) + message(128) = 140 bytes
    offset = 0;

    // PacketHeader: type(1) + player_id(4) + payload_size(2) = 7 bytes
    offset++; // skip type

    uint32_t header_player_id_net;
    memcpy(&header_player_id_net, &response_buffer[offset], 4);
    offset += 4;
    uint32_t player_id = ntohl(header_player_id_net);

    offset += 2;  // skip payload_size

    // Response fields start at offset 7
    uint8_t success = response_buffer[offset++];

    // Player ID in payload
    uint32_t payload_player_id_net;
    memcpy(&payload_player_id_net, &response_buffer[offset], 4);
    offset += 4;

    // Message (rest of packet)
    char message[256] = {0};
    int message_len = received - offset;
    if (message_len < 0) message_len = 0;
    if (message_len > 255) message_len = 255;
    if (message_len > 0) {
        memcpy(message, &response_buffer[offset], message_len);
        message[message_len] = '\0';
    }
    
    printf("[REGISTER] Parsed: success=%d, player_id=%u, message='%s'\n", 
           success, player_id, message);
    
    if (success) {
        *out_player_id = player_id;
        printf("[REGISTER] SUCCESS! New player ID: %u\n", player_id);
        
        if (message_len > 0) {
            strncpy(errorMsg, message, errorMsgSize - 1);
            errorMsg[errorMsgSize - 1] = '\0';
        } else {
            snprintf(errorMsg, errorMsgSize, "Registration successful");
        }
        
        return TRUE;
    } else {
        printf("[REGISTER] FAILED: %s\n", message);
        
        if (message_len > 0) {
            strncpy(errorMsg, message, errorMsgSize - 1);
            errorMsg[errorMsgSize - 1] = '\0';
        } else {
            snprintf(errorMsg, errorMsgSize, "Registration failed");
        }
        
        return FALSE;
    }
}
