#include "config.h"

ServerConfig g_server;
ServerState g_state;

void signal_handler(int signum) {
    switch (signum) {
        case SIGINT: 
        case SIGTERM:
        case SIGQUIT:
            printf("\nReceived shutdown signal (%d)\n", signum);
            g_server.running = 0;
            break;
        default:
            break;
    } 
} 

void setup_signals(void) {
    signal(SIGPIPE, SIG_IGN);
    
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGQUIT, &sa, NULL);
}

int create_tcp_server_socket(int port) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        printf("Socket Creation Failed\n");
        return -1;
    }
    
    int opt = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        printf("setsockopt SO_REUSEADDR failed\n");
        close(sock);
        return -1;
    }
    
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(port);
    
    if (bind(sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        printf("Bind Failed\n");
        close(sock);
        return -1;
    }
    
    if (listen(sock, MAX_PENDING_CONNECTIONS) < 0) {
        printf("Listen Failed\n");
        close(sock);
        return -1;
    }
    
    printf("TCP server socket bound to port %d\n", port);
    return sock;
}

int set_config(const char* filepath) {
    FILE* file = fopen(filepath, "r");
    if (!file) {
        fprintf(stderr, "Failed to open config file: %s\n", filepath);
        return 0;
    }

    char line[256];
    int field_count = 0;
    
    // Initialize defaults
    memset(g_server.name, 0, sizeof(g_server.name));
    g_server.port = REALM_SERVER_PORT;  // Default port
    
    while (fgets(line, sizeof(line), file)) {
        // Remove trailing newline/whitespace
        line[strcspn(line, "\r\n")] = 0;
        
        // Skip empty lines
        if (strlen(line) == 0) continue;
        
        // Skip comment lines that start with #
        if (line[0] == '#') continue;
        
        // Trim leading whitespace
        char* value = line;
        while (*value && isspace(*value)) value++;
        
        // Skip if empty after trimming
        if (strlen(value) == 0) continue;
        
        // Parse based on field order
        if (field_count == 0) {
            // Server Name
            strncpy(g_server.name, value, sizeof(g_server.name) - 1);
            field_count++;
        } else if (field_count == 1) {
            // Port
            int port = atoi(value);
            if (port <= 0 || port > 65535) {
                fprintf(stderr, "Invalid port: %s\n", value);
                fclose(file);
                return 0;
            }
            g_server.port = port;
            field_count++;
            break;  // All fields parsed for realm server
        }
    }
    
    fclose(file);
    
    // Verify required fields were read
    if (field_count != 2) {
        fprintf(stderr, "Incomplete config file. Expected 2 fields, got %d\n", field_count);
        return 0;
    }
    
    // Validation
    if (strlen(g_server.name) == 0) {
        fprintf(stderr, "Server name cannot be empty\n");
        return 0;
    }
    
    if (g_server.port == 0) {
        fprintf(stderr, "Port cannot be 0\n");
        return 0;
    }
    
    // Success - print loaded config
    printf("=== Realm Server Configuration ===\n");
    printf("Server Name: %s\n", g_server.name);
    printf("Port: %d\n", g_server.port);
    printf("===================================\n");
    
    return 1;  // Success
}