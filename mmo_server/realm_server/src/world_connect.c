/**
 * @file
 * Authenticate realm-to-world connections and load configured world endpoints.
 */
#include "world_connect.h"

/**
 * Send an entire buffer unless the socket fails.
 *
 * @return      Nonzero when every byte is sent, otherwise zero.
 */
static int send_exact(int fd, const void* buffer, size_t length) {
    const uint8_t* ptr = buffer;
    size_t total = 0;
    while (total < length) {
        ssize_t sent = send(fd, ptr + total, length - total, 0);
        if (sent <= 0) return 0;
        total += (size_t)sent;
    }
    return 1;
}

/**
 * Receive an exact byte count with a timeout applied to each poll.
 *
 * @return      Nonzero when every byte is received, otherwise zero.
 */
static int recv_exact_timeout(int fd, void* buffer, size_t length, int timeout_ms) {
    uint8_t* ptr = buffer;
    size_t total = 0;
    while (total < length) {
        struct pollfd pfd = {.fd = fd, .events = POLLIN};
        if (poll(&pfd, 1, timeout_ms) <= 0 || !(pfd.revents & POLLIN)) return 0;
        ssize_t got = recv(fd, ptr + total, length - total, 0);
        if (got <= 0) return 0;
        total += (size_t)got;
    }
    return 1;
}

/**
 * Connect to a world endpoint and complete server-key authentication.
 *
 * @param silent  Nonzero to suppress connection diagnostics.
 * @return        The authenticated socket descriptor, or -1 on failure.
 */
int connect_to_world_server(const char* host, int port, const char* server_key, int silent) {
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        if (!silent) perror("socket");
        return -1;
    }
    
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    
    if (inet_pton(AF_INET, host, &addr.sin_addr) <= 0) {
        if (!silent) perror("inet_pton");
        close(sockfd); 
        return -1;
    }
    
    if (connect(sockfd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        if (!silent) perror("connect");
        close(sockfd);
        return -1;
    }
    
    // Authenticate with server key
    RealmAuthPacket auth = {0};
    auth.header.type = PACKET_REALM_AUTH;
    auth.header.player_id = 0;
    auth.header.payload_size = 0;
    strncpy(auth.server_key, server_key, 63);
    snprintf(auth.server_key, sizeof(auth.server_key), "%s", server_key);
    
    if (!send_exact(sockfd, &auth, sizeof(auth))) {
        if (!silent) perror("send auth");
        close(sockfd);
        return -1;
    }
    
    // Wait for auth response
    RealmAuthAckPacket ack;
    struct pollfd pfd = {.fd = sockfd, .events = POLLIN};
    if (poll(&pfd, 1, 5000) <= 0) {
        if (!silent) printf("Auth timeout\n");
        close(sockfd);
        return -1;
    }
    
    if (!recv_exact_timeout(sockfd, &ack, sizeof(ack), 5000)) {
        if (!silent) perror("recv auth ack");
        close(sockfd);
        return -1;
    }
    
    if (ack.header.type != PACKET_REALM_AUTH_ACK || !ack.success) {
        if (!silent) printf("Auth failed: %s\n", ack.message);
        close(sockfd);
        return -1;
    }
    
    if (!silent) printf("Authenticated to world server: %s\n", ack.message);
    return sockfd;
}

/**
 * Parse world endpoint records and their region headings from a text file.
 *
 * @return      The number of loaded worlds, or -1 when the file cannot be opened.
 */
int load_world_servers_from_file(const char* filepath, WorldServer* servers, int max_servers) {
    FILE* file = fopen(filepath, "r");
    if (!file) {
        perror("Failed to open world config file");
        return -1;
    }
    
    char line[256];
    int count = 0;
    char current_region[64] = "Unknown";
    
    while (fgets(line, sizeof(line), file) && count < max_servers) {
        // Remove newline
        line[strcspn(line, "\n")] = 0;
        
        // Skip empty lines
        if (strlen(line) == 0) continue;
        
        // Check if this is a region header (contains '-')
        if (strstr(line, " - ")) {
            // Extract region name (everything before " - ")
            char* dash = strstr(line, " - ");
            size_t region_len = dash - line;
            if (region_len < sizeof(current_region)) {
                strncpy(current_region, line, region_len);
                current_region[region_len] = '\0';
                
                // Trim trailing spaces
                for (int i = region_len - 1; i >= 0 && isspace(current_region[i]); i--) {
                    current_region[i] = '\0';
                }
            }
            continue;
        }
        
        // Parse world server line: "Name IP Port"
        char name[64], ip[64];
        int port;
        
        if (sscanf(line, "%63s %63s %d", name, ip, &port) == 3) {
            WorldServer* ws = &servers[count];
            
            snprintf(ws->name, sizeof(ws->name), "%s", name);
            snprintf(ws->host, sizeof(ws->host), "%s", ip);
            ws->port = port;
            ws->fd = -1;
            ws->online = 0;
            ws->player_count = 0;
            ws->max_players = 1000;
            ws->last_heartbeat = 0;
            ws->connection_logged = 0;  // Initialize the new field
            
            printf("Loaded world server: %s (%s) at %s:%d\n", 
                   ws->name, current_region, ws->host, ws->port);
            
            count++;
        }
    }
    
    fclose(file);
    printf("Loaded %d world servers from config\n", count);
    return count;
}
