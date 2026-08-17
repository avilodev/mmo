/**
 * @file
 * Fetch and display patch notes within the launcher window.
 */
#include "ui_patch_notes.h"
#include "network.h"
#include "tls_client.h"
#include "protocol.h"
#include <stdio.h>
#include <string.h>

static HWND g_hwndPatchNotesPanel = NULL;
static HWND g_hwndPatchNotesText = NULL;
static BOOL g_patchNotesVisible = FALSE;
static BOOL g_patchNotesFetched = FALSE;
static char g_patchNotesContent[4096] = "Loading patch notes...";

/**
 * Fetch a bounded patch-note range from the login server over TLS.
 *
 * This call blocks and decodes a seven-byte MMO packet header in network byte order.
 *
 * @return      TRUE when patch-note content is received, otherwise FALSE with buffer populated.
 */
static BOOL FetchPatchNotes(char* buffer, int bufferSize) {
    SOCKET sock = INVALID_SOCKET;
    struct sockaddr_in server_addr;
    uint8_t request_buffer[64];  // Manual buffer for request
    uint8_t response_buffer[4096];  // Manual buffer for response
    int wsaError; 
    
    printf("[PATCH NOTES] Starting fetch from 127.0.0.1:7776\n");
    
    // Create socket
    sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) {
        wsaError = WSAGetLastError();
        printf("[PATCH NOTES] Failed to create socket: %d\n", wsaError);
        snprintf(buffer, bufferSize, "Failed to create socket (Error: %d)", wsaError);
        return FALSE;
    }
    
    printf("[PATCH NOTES] Socket created successfully\n");
    
    // Set timeout (10 seconds for testing)
    DWORD timeout = 10000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (char*)&timeout, sizeof(timeout));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (char*)&timeout, sizeof(timeout));
    
    // Setup server address
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons((u_short)g_login_server_port);

    if (inet_pton(AF_INET, g_login_server_ip, &server_addr.sin_addr) <= 0) {
        printf("[PATCH NOTES] Invalid IP address format\n");
        snprintf(buffer, bufferSize, "Invalid server IP address");
        closesocket(sock);
        return FALSE;
    }

    printf("[PATCH NOTES] Connecting to %s:%d...\n", g_login_server_ip, g_login_server_port);

    // Connect to server
    if (connect(sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        wsaError = WSAGetLastError();
        printf("[PATCH NOTES] Connection failed: %d\n", wsaError);
        snprintf(buffer, bufferSize, "Cannot connect to server at %s:%d\nError: %d",
                 g_login_server_ip, g_login_server_port, wsaError);
        closesocket(sock);
        return FALSE;
    }

    printf("[PATCH NOTES] Connected successfully!\n");

    // TLS handshake
    SSL* ssl = tls_client_connect(sock);
    if (!ssl) {
        snprintf(buffer, bufferSize, "TLS handshake failed");
        closesocket(sock);
        return FALSE;
    }

    // Small delay to ensure server is ready
    Sleep(100);
    
    // Build packet manually to avoid struct padding issues
    int offset = 0;
    
    // PacketHeader: type(1) + player_id(4) + payload_size(2) = 7 bytes
    request_buffer[offset++] = PATCH_NOTES_REQUEST;  // type
    
    uint32_t player_id_net = htonl(0);
    memcpy(&request_buffer[offset], &player_id_net, 4);
    offset += 4;
    
    uint16_t payload_size = 4;  // start(2) + end(2) = 4 bytes
    uint16_t payload_size_net = htons(payload_size);
    memcpy(&request_buffer[offset], &payload_size_net, 2);
    offset += 2;
    
    // Payload: start and end
    uint16_t start = htons(0);
    memcpy(&request_buffer[offset], &start, 2);
    offset += 2;
    
    uint16_t end = htons(20);
    memcpy(&request_buffer[offset], &end, 2);
    offset += 2;
    
    int packet_size = offset;  // Should be 11 bytes total
    
    printf("[PATCH NOTES] Sending request packet (type=%d, size=%d bytes)...\n", 
           PATCH_NOTES_REQUEST, packet_size);
    printf("[PATCH NOTES] Packet breakdown: header(7) + start(2) + end(2) = %d bytes\n", packet_size);
    
    // Send request
    int sent = tls_client_send(ssl, (char*)request_buffer, packet_size);
    if (sent != packet_size) {
        wsaError = WSAGetLastError();
        printf("[PATCH NOTES] Send failed: sent=%d, expected=%d, error=%d\n",
               sent, packet_size, wsaError);
        snprintf(buffer, bufferSize, "Failed to send patch notes request\nSent: %d bytes (expected %d)\nError: %d",
                 sent, packet_size, wsaError);
        tls_client_close(ssl);
        closesocket(sock);
        return FALSE;
    }

    printf("[PATCH NOTES] Request sent successfully (%d bytes)\n", sent);
    printf("[PATCH NOTES] Waiting for response...\n");

    // Receive response
    memset(response_buffer, 0, sizeof(response_buffer));
    int received = tls_client_recv_packet(ssl, (char*)response_buffer, sizeof(response_buffer));
    printf("[PATCH NOTES] Received %d bytes\n", received);

    if (received <= 0) {
        wsaError = WSAGetLastError();
        printf("[PATCH NOTES] Receive failed: received=%d, error=%d\n", received, wsaError);
        snprintf(buffer, bufferSize, "Failed to receive response from server\nReceived: %d bytes\nError: %d",
                 received, wsaError);
        tls_client_close(ssl);
        closesocket(sock);
        return FALSE;
    }

    if (received < 7) {  // Minimum header size
        printf("[PATCH NOTES] Response too small: %d bytes\n", received);
        snprintf(buffer, bufferSize, "Invalid response size: %d bytes", received);
        tls_client_close(ssl);
        closesocket(sock);
        return FALSE;
    }

    tls_client_close(ssl);
    closesocket(sock);
    
    // Parse response manually
    offset = 0;
    
    // PacketHeader
    uint8_t response_type = response_buffer[offset++];
    offset += 4;  // skip player_id
    
    uint16_t response_payload_size_net;
    memcpy(&response_payload_size_net, &response_buffer[offset], 2);
    offset += 2;
    uint16_t response_payload_size = ntohs(response_payload_size_net);
    
    printf("[PATCH NOTES] Response: type=%d (expected %d), payload_size=%d\n", 
           response_type, PATCH_NOTES_RESPONSE, response_payload_size);
    
    // Check if we got patch notes response
    if (response_type == PATCH_NOTES_RESPONSE) {
        // Extract patch notes content (rest of packet)
        int content_length = received - offset;
        if (content_length < 0) content_length = 0;
        if (content_length > bufferSize - 1) content_length = bufferSize - 1;
        
        if (content_length > 0) {
            memcpy(buffer, &response_buffer[offset], content_length);
            buffer[content_length] = '\0';
            
            printf("[PATCH NOTES] Success! Received %d bytes of content\n", content_length);
            printf("[PATCH NOTES] Content preview: %.50s...\n", buffer);
        } else {
            printf("[PATCH NOTES] Warning: No content in response\n");
            buffer[0] = '\0';
        }
        
        return TRUE;
    } else {
        printf("[PATCH NOTES] Invalid response type received\n");
        snprintf(buffer, bufferSize, "Invalid response from server (got type %d, expected %d)", 
                 response_type, PATCH_NOTES_RESPONSE);
        return FALSE;
    }
}

/** Create the initially hidden patch-note panel and read-only text control. */
void CreatePatchNotesPanel(HWND hwndParent) {
    // Create invisible panel initially
    g_hwndPatchNotesPanel = CreateWindowEx(
        0,
        "STATIC",
        "",
        WS_CHILD,
        0, HEADER_HEIGHT,
        WINDOW_WIDTH, WINDOW_HEIGHT - HEADER_HEIGHT - STATUS_BAR_HEIGHT,
        hwndParent,
        (HMENU)ID_PATCH_NOTES_PANEL,
        GetModuleHandle(NULL),
        NULL
    );
    
    // Create multiline edit control for patch notes - full height
    g_hwndPatchNotesText = CreateWindowEx(
        0,  // Removed WS_EX_CLIENTEDGE for cleaner look
        "EDIT",
        "",
        WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL | 
        ES_READONLY | WS_VSCROLL,
        20, 20,
        WINDOW_WIDTH - 40, WINDOW_HEIGHT - HEADER_HEIGHT - STATUS_BAR_HEIGHT - 40,
        g_hwndPatchNotesPanel,
        (HMENU)ID_PATCH_NOTES_TEXT,
        GetModuleHandle(NULL),
        NULL
    );
    
    // Set font for patch notes text
    HFONT hFont = CreateFont(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                            CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                            DEFAULT_PITCH | FF_DONTCARE, "Consolas");
    SendMessage(g_hwndPatchNotesText, WM_SETFONT, (WPARAM)hFont, TRUE);
}

/** Show cached patch notes or fetch and convert them for a Windows edit control. */
void ShowPatchNotes(HWND hwndParent) {
    if (!g_hwndPatchNotesPanel) {
        CreatePatchNotesPanel(hwndParent);
    }
    
    // Hide login panel (username/password fields)
    HideLoginPanel();
    
    // Hide the welcome label and start game button too
    HideStartGameButton();
    
    // Only fetch from server once per launcher session
    if (g_patchNotesFetched) {
        SetWindowText(g_hwndPatchNotesText, g_patchNotesContent);
        ShowWindow(g_hwndPatchNotesPanel, SW_SHOW);
        g_patchNotesVisible = TRUE;
        return;
    }

    // Fetch patch notes from server
    char buffer[4096];
    if (FetchPatchNotes(buffer, sizeof(buffer))) {
        // Process the buffer to ensure proper line breaks
        // Windows edit controls require \r\n for new lines, not just \n
        char processed[4096];
        int j = 0;
        
        for (int i = 0; buffer[i] != '\0' && j < (int)(sizeof(processed) - 2); i++) {
            if (buffer[i] == '\n') {
                // Convert standalone \n to \r\n for Windows edit control
                processed[j++] = '\r';
                processed[j++] = '\n';
            } else if (buffer[i] == '\r' && buffer[i + 1] == '\n') {
                // Already \r\n, copy both characters
                processed[j++] = buffer[i++];
                processed[j++] = buffer[i];
            } else if (buffer[i] != '\r') {
                // Skip standalone \r to avoid issues
                processed[j++] = buffer[i];
            }
        }
        processed[j] = '\0';
        
        strcpy(g_patchNotesContent, processed);
        g_patchNotesFetched = TRUE;
    } else {
        snprintf(g_patchNotesContent, sizeof(g_patchNotesContent), 
                "Failed to load patch notes:\r\n\r\n%s", buffer);
    }
    
    // Update text control
    SetWindowText(g_hwndPatchNotesText, g_patchNotesContent);
    
    // Show panel
    ShowWindow(g_hwndPatchNotesPanel, SW_SHOW);
    g_patchNotesVisible = TRUE;
}

/** Hide patch notes and restore the appropriate login-panel state. */
void HidePatchNotes(void) {
    if (g_hwndPatchNotesPanel) {
        ShowWindow(g_hwndPatchNotesPanel, SW_HIDE);
        g_patchNotesVisible = FALSE;
    }
    
    // Show appropriate login panel (login form or logged-in state)
    ShowLoginPanel();
}

/**
 * Report whether the patch-note panel is visible.
 *
 * @return      TRUE while the panel is visible, otherwise FALSE.
 */
BOOL IsPatchNotesVisible(void) {
    return g_patchNotesVisible;
}

/** Accept the shared command-dispatch interface for the buttonless patch-note panel. */
void HandlePatchNotesCommand(HWND hwnd, int controlId) {
    (void)hwnd;
    (void)controlId;
    // No buttons to handle anymore - use Home tab to go back
}
