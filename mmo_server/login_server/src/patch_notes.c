/**
 * @file
 * Cache login-server patch notes and serve them in protocol responses.
 */
#include "patch_notes.h"
#include "tls.h"

static char g_patch_notes_cache[PATCH_NOTES_MAX_SIZE] = {0};
static size_t g_patch_notes_len = 0;

/** Load patch-note text into the process-wide response cache. */
void patch_notes_init(void) {
    FILE* file = fopen(PATCH_NOTES_PATH, "r");
    if (file == NULL) {
        printf("[PATCH NOTES] Warning: could not open %s — serving empty notes\n", PATCH_NOTES_PATH);
        snprintf(g_patch_notes_cache, sizeof(g_patch_notes_cache), "No patch notes available.");
        g_patch_notes_len = strlen(g_patch_notes_cache);
        return;
    }

    g_patch_notes_len = fread(g_patch_notes_cache, 1, sizeof(g_patch_notes_cache) - 1, file);
    g_patch_notes_cache[g_patch_notes_len] = '\0';
    fclose(file);

    printf("[PATCH NOTES] Cached %zu bytes from %s\n", g_patch_notes_len, PATCH_NOTES_PATH);
}

/**
 * Send the cached patch notes to a TLS client.
 *
 * The response uses a seven-byte header with network-order player and payload fields.
 */
void handle_patch_notes_request(int client_fd, PacketHeader* packet, ssize_t bytes) {
    (void)packet;
    (void)bytes;

    printf("[PATCH NOTES] Serving %zu cached bytes\n", g_patch_notes_len);

    uint8_t send_buffer[7 + PATCH_NOTES_MAX_SIZE];
    int offset = 0;

    send_buffer[offset++] = PATCH_NOTES_RESPONSE;

    uint32_t player_id_net = htonl(0);
    memcpy(&send_buffer[offset], &player_id_net, 4);
    offset += 4;

    uint16_t payload_size_net = htons((uint16_t)g_patch_notes_len);
    memcpy(&send_buffer[offset], &payload_size_net, 2);
    offset += 2;

    memcpy(&send_buffer[offset], g_patch_notes_cache, g_patch_notes_len);
    offset += (int)g_patch_notes_len;

    ssize_t sent = tls_send(client_fd, send_buffer, offset, 0);
    if (sent < 0) {
        perror("[PATCH NOTES] Failed to send response");
    } else if (sent != offset) {
        printf("[PATCH NOTES] Warning: partial send (%zd/%d bytes)\n", sent, offset);
    }
}
