#include "patch_notes.h"

void handle_patch_notes_request(int client_fd, PacketHeader* packet, ssize_t bytes) {
    if (bytes < MIN_HEADER_SIZE) {
        printf("Packet too small for patch notes request\n");
        return;
    }

    // Parse the request to get start/end (if you need them)
    int offset = MIN_HEADER_SIZE;
    uint16_t start_net, end_net;
    memcpy(&start_net, (uint8_t*)packet + offset, 2);
    offset += 2;
    memcpy(&end_net, (uint8_t*)packet + offset, 2);
    offset += 2;
    
    uint16_t start = ntohs(start_net);
    uint16_t end = ntohs(end_net);
    
    printf("[PATCH NOTES] Received request: start=%u, end=%u\n", start, end);

    // Read patch notes from file
    char patch_notes_content[4000];
    memset(patch_notes_content, 0, sizeof(patch_notes_content));
    
    FILE* file = fopen(PATCH_NOTES_PATH, "r");
    if (file == NULL) {
        printf("[PATCH NOTES] Failed to open patch notes file at %s\n", PATCH_NOTES_PATH);
        snprintf(patch_notes_content, sizeof(patch_notes_content), 
                 "Error: Patch notes file not found");
    } else {
        // Read up to 3999 bytes (leaving room for null terminator)
        size_t bytes_read = fread(patch_notes_content, 1, sizeof(patch_notes_content) - 1, file);
        patch_notes_content[bytes_read] = '\0';  // Null terminate 
        
        fclose(file);
        printf("[PATCH NOTES] Read %zu bytes from file\n", bytes_read);
    }

    // Build response manually to avoid sending unnecessary zeros
    uint8_t send_buffer[4096];
    int send_offset = 0;
    
    // PacketHeader: type(1) + player_id(4) + payload_size(2)
    send_buffer[send_offset++] = PATCH_NOTES_RESPONSE;
    
    uint32_t player_id_net = htonl(0);
    memcpy(&send_buffer[send_offset], &player_id_net, 4);
    send_offset += 4;
    
    // Calculate actual content length
    size_t content_len = strlen(patch_notes_content);
    uint16_t payload_size_net = htons(content_len);
    memcpy(&send_buffer[send_offset], &payload_size_net, 2);
    send_offset += 2;
    
    // Copy actual content (not the full 4000 byte buffer)
    memcpy(&send_buffer[send_offset], patch_notes_content, content_len);
    send_offset += content_len;
    
    printf("[PATCH NOTES] Sending %d bytes (header: 7, content: %zu)\n", 
           send_offset, content_len);
    
    // Send only the bytes we need
    ssize_t sent = send(client_fd, send_buffer, send_offset, 0);
    if (sent < 0) {
        perror("[PATCH NOTES] Failed to send response");
    } else if (sent != send_offset) {
        printf("[PATCH NOTES] Warning: Partial send (%zd/%d bytes)\n", sent, send_offset);
    } else {
        printf("[PATCH NOTES] Successfully sent %zd bytes to client\n", sent);
    }
}