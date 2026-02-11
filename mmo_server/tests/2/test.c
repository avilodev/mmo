#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>

// Simplified packet definitions for testing
#define PACKET_REALM_AUTH 200
#define PACKET_REALM_AUTH_ACK 201
#define PACKET_WORLD_HEARTBEAT 202
#define PACKET_WORLD_STATUS 203

typedef struct {
    uint8_t type;
    uint32_t player_id;
    uint16_t payload_size;
} PacketHeader;

typedef struct {
    PacketHeader header;
    char server_key[64];
    char realm_name[32];
} RealmAuthPacket;

typedef struct {
    PacketHeader header;
    uint8_t success;
    char message[64];
} RealmAuthAckPacket;

typedef struct {
    PacketHeader header;
    uint64_t timestamp;
} WorldHeartbeatPacket;

typedef struct {
    PacketHeader header;
    char server_name[64];
    uint32_t player_count;
    uint32_t max_players;
    uint8_t status;
    float cpu_usage;
    uint64_t uptime;
} WorldStatusPacket;

// Test counters
int test_passed = 0;
int test_failed = 0;

void print_test_result(const char* test_name, int passed) {
    if (passed) {
        printf("✓ PASS: %s\n", test_name);
        test_passed++;
    } else {
        printf("✗ FAIL: %s\n", test_name);
        test_failed++;
    }
}

// Simulate a world server
void* mock_world_server(void* arg) {
    int port = *(int*)arg;
    free(arg);
    
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
    
    if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("Mock world server bind failed");
        return NULL;
    }
    
    listen(server_fd, 1);
    printf("Mock world server listening on port %d\n", port);
    
    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);
    int client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &addr_len);
    
    if (client_fd < 0) {
        perror("Accept failed");
        close(server_fd);
        return NULL;
    }
    
    printf("Mock world server: Client connected\n");
    
    // Receive auth packet
    RealmAuthPacket auth;
    if (recv(client_fd, &auth, sizeof(auth), 0) <= 0) {
        printf("Mock world server: Failed to receive auth\n");
        close(client_fd);
        close(server_fd);
        return NULL;
    }
    
    printf("Mock world server: Received auth from '%s' with key '%s'\n", 
           auth.realm_name, auth.server_key);
    
    // Validate key
    int valid = (strcmp(auth.server_key, "test-key-12345") == 0);
    
    // Send auth response
    RealmAuthAckPacket ack = {0};
    ack.header.type = PACKET_REALM_AUTH_ACK;
    ack.success = valid ? 1 : 0;
    strncpy(ack.message, valid ? "Authenticated" : "Invalid key", 63);
    
    send(client_fd, &ack, sizeof(ack), 0);
    printf("Mock world server: Sent auth ack (success=%d)\n", ack.success);
    
    if (!valid) {
        close(client_fd);
        close(server_fd);
        return NULL;
    }
    
    // Handle heartbeats
    for (int i = 0; i < 3; i++) {
        WorldHeartbeatPacket hb;
        if (recv(client_fd, &hb, sizeof(hb), 0) <= 0) {
            printf("Mock world server: Heartbeat recv failed\n");
            break;
        }
        
        printf("Mock world server: Received heartbeat #%d\n", i + 1);
        
        // Send status
        WorldStatusPacket status = {0};
        status.header.type = PACKET_WORLD_STATUS;
        status.player_count = htonl(42 + i);
        status.max_players = htonl(1000);
        status.status = 1;
        status.uptime = 12345;
        strncpy(status.server_name, "TestWorld", 63);
        
        send(client_fd, &status, sizeof(status), 0);
        printf("Mock world server: Sent status response\n");
    }
    
    close(client_fd);
    close(server_fd);
    printf("Mock world server: Shutting down\n");
    return NULL;
}

// Test 1: Basic packet structure sizes
void test_packet_sizes() {
    int passed = 1;
    
    if (sizeof(PacketHeader) != 7) {
        printf("  Expected PacketHeader size: 7, got: %zu\n", sizeof(PacketHeader));
        passed = 0;
    }
    
    if (sizeof(RealmAuthPacket) < 90) {
        printf("  RealmAuthPacket too small: %zu\n", sizeof(RealmAuthPacket));
        passed = 0;
    }
    
    print_test_result("Packet structure sizes", passed);
}

// Test 2: Connect to mock world server
void test_connection_to_world_server() {
    int port = 9999;
    
    // Start mock world server
    pthread_t server_thread;
    int* port_ptr = malloc(sizeof(int));
    *port_ptr = port;
    pthread_create(&server_thread, NULL, mock_world_server, port_ptr);
    
    sleep(1); // Let server start
    
    // Connect as realm server
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    
    int connected = (connect(sockfd, (struct sockaddr*)&addr, sizeof(addr)) == 0);
    
    if (connected) {
        // Send auth
        RealmAuthPacket auth = {0};
        auth.header.type = PACKET_REALM_AUTH;
        strncpy(auth.server_key, "test-key-12345", 63);
        strncpy(auth.realm_name, "TestRealm", 31);
        
        send(sockfd, &auth, sizeof(auth), 0);
        
        // Receive ack
        RealmAuthAckPacket ack;
        recv(sockfd, &ack, sizeof(ack), 0);
        
        int passed = (ack.success == 1 && ack.header.type == PACKET_REALM_AUTH_ACK);
        print_test_result("Realm-World authentication", passed);
        
        // Send heartbeats
        int all_heartbeats_ok = 1;
        for (int i = 0; i < 3; i++) {
            WorldHeartbeatPacket hb = {0};
            hb.header.type = PACKET_WORLD_HEARTBEAT;
            hb.timestamp = time(NULL);
            
            if (send(sockfd, &hb, sizeof(hb), 0) <= 0) {
                all_heartbeats_ok = 0;
                break;
            }
            
            WorldStatusPacket status;
            if (recv(sockfd, &status, sizeof(status), 0) <= 0) {
                all_heartbeats_ok = 0;
                break;
            }
            
            if (status.header.type != PACKET_WORLD_STATUS) {
                all_heartbeats_ok = 0;
                break;
            }
            
            printf("  Heartbeat %d: %u/%u players\n", 
                   i + 1, ntohl(status.player_count), ntohl(status.max_players));
        }
        
        print_test_result("Heartbeat exchange (3 rounds)", all_heartbeats_ok);
        
        close(sockfd);
    } else {
        print_test_result("Realm-World authentication", 0);
        print_test_result("Heartbeat exchange (3 rounds)", 0);
    }
    
    pthread_join(server_thread, NULL);
}

// Test 3: Invalid auth key
void test_invalid_auth_key() {
    int port = 9998;
    
    // Start mock world server
    pthread_t server_thread;
    int* port_ptr = malloc(sizeof(int));
    *port_ptr = port;
    pthread_create(&server_thread, NULL, mock_world_server, port_ptr);
    
    sleep(1);
    
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    
    if (connect(sockfd, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
        // Send auth with WRONG key
        RealmAuthPacket auth = {0};
        auth.header.type = PACKET_REALM_AUTH;
        strncpy(auth.server_key, "wrong-key", 63);
        strncpy(auth.realm_name, "TestRealm", 31);
        
        send(sockfd, &auth, sizeof(auth), 0);
        
        // Receive ack
        RealmAuthAckPacket ack;
        recv(sockfd, &ack, sizeof(ack), 0);
        
        int passed = (ack.success == 0 && ack.header.type == PACKET_REALM_AUTH_ACK);
        print_test_result("Invalid key rejection", passed);
        
        close(sockfd);
    } else {
        print_test_result("Invalid key rejection", 0);
    }
    
    pthread_join(server_thread, NULL);
}

// Test 4: Packet type validation
void test_packet_types() {
    int passed = 1;
    
    if (PACKET_REALM_AUTH != 200) passed = 0;
    if (PACKET_REALM_AUTH_ACK != 201) passed = 0;
    if (PACKET_WORLD_HEARTBEAT != 202) passed = 0;
    if (PACKET_WORLD_STATUS != 203) passed = 0;
    
    print_test_result("Packet type constants", passed);
}

// Test 5: Load world servers from config file
typedef struct {
    char name[64];
    char host[64];
    uint16_t port;
    int fd;
    uint8_t online;
    uint32_t player_count;
    uint32_t max_players;
    time_t last_heartbeat;
} WorldServer;

int load_world_servers_from_file(const char* filepath, WorldServer* servers, int max_servers) {
    FILE* file = fopen(filepath, "r");
    if (!file) {
        return -1;
    }
    
    char line[256];
    int count = 0;
    char current_region[64] = "Unknown";
    
    while (fgets(line, sizeof(line), file) && count < max_servers) {
        line[strcspn(line, "\n")] = 0;
        
        if (strlen(line) == 0) continue;
        
        if (strstr(line, " - ")) {
            char* dash = strstr(line, " - ");
            size_t region_len = dash - line;
            if (region_len < sizeof(current_region)) {
                strncpy(current_region, line, region_len);
                current_region[region_len] = '\0';
            }
            continue;
        }
        
        char name[64], ip[64];
        int port;
        
        if (sscanf(line, "%63s %63s %d", name, ip, &port) == 3) {
            WorldServer* ws = &servers[count];
            snprintf(ws->name, sizeof(ws->name), "%s", name);
            snprintf(ws->host, sizeof(ws->host), "%s", ip);
            ws->port = port;
            ws->fd = -1;
            ws->online = 0;
            count++;
        }
    }
    
    fclose(file);
    return count;
}

void test_world_config_loading() {
    WorldServer servers[20];
    
    const char* config_path = getenv("WORLD_FILE_PATH");
    if (!config_path) {
        config_path = "worlds.conf";
    }
    
    int count = load_world_servers_from_file(config_path, servers, 20);
    
    if (count > 0) {
        printf("  Loaded %d world servers:\n", count);
        for (int i = 0; i < count; i++) {
            printf("    - %s at %s:%d\n", servers[i].name, servers[i].host, servers[i].port);
        }
    }
    
    // Expected: 10 servers (6 NA + 3 EU + 1 Asia)
    int passed = (count == 10);
    
    // Verify specific servers exist
    if (passed) {
        int found_armeia = 0, found_karmel = 0, found_jatrus = 0;
        for (int i = 0; i < count; i++) {
            if (strcmp(servers[i].name, "Armeia") == 0 && servers[i].port == 7778) found_armeia = 1;
            if (strcmp(servers[i].name, "Karmel") == 0 && servers[i].port == 7785) found_karmel = 1;
            if (strcmp(servers[i].name, "Jatrus") == 0 && servers[i].port == 7788) found_jatrus = 1;
        }
        passed = (found_armeia && found_karmel && found_jatrus);
    }
    
    print_test_result("World config file loading", passed);
}

int main() {
    printf("=================================\n");
    printf("REALM-WORLD SERVER COMMUNICATION TEST\n");
    printf("=================================\n\n");
    
    test_packet_sizes();
    test_packet_types();
    test_world_config_loading();
    test_connection_to_world_server();
    test_invalid_auth_key();
    
    printf("\n=================================\n");
    printf("TEST SUMMARY\n");
    printf("=================================\n");
    printf("Passed: %d\n", test_passed);
    printf("Failed: %d\n", test_failed);
    printf("Total:  %d\n", test_passed + test_failed);
    
    if (test_failed == 0) {
        printf("\n✓ ALL TESTS PASSED!\n");
        return 0;
    } else {
        printf("\n✗ SOME TESTS FAILED\n");
        return 1;
    }
}