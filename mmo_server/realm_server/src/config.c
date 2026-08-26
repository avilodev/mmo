/**
 * @file
 * Configure realm-server signals, listening sockets, and runtime identity.
 */
#include "config.h"
#include "log.h"
#include "str_fixed.h"

ServerConfig g_server;
ServerState g_state;

/** Request realm-server shutdown for supported termination signals. */
void signal_handler(int signum) {
    switch (signum) {
        case SIGINT: 
        case SIGTERM:
        case SIGQUIT:
            LOG_INFO("\nReceived shutdown signal (%d)", signum);
            g_server.running = 0;
            break;
        default:
            break;
    } 
} 

/** Install realm shutdown handlers and ignore broken-pipe signals. */
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

/**
 * Create, bind, and listen on a reusable IPv4 TCP socket.
 *
 * @return      The listening descriptor, or -1 when setup fails.
 */
int create_tcp_server_socket(int port) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        LOG_INFO("Socket Creation Failed");
        return -1;
    }
    
    int opt = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        LOG_INFO("setsockopt SO_REUSEADDR failed");
        close(sock);
        return -1;
    }
    
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(port);
    
    if (bind(sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        LOG_INFO("Bind Failed");
        close(sock);
        return -1;
    }
    
    if (listen(sock, MAX_PENDING_CONNECTIONS) < 0) {
        LOG_INFO("Listen Failed");
        close(sock);
        return -1;
    }
    
    LOG_INFO("TCP server socket bound to port %d", port);
    return sock;
}

/**
 * Load the realm name and listening port from an ordered two-field configuration file.
 *
 * @return      Nonzero when both fields are valid, otherwise zero.
 */
int set_config(const char* filepath) {
    FILE* file = fopen(filepath, "r");
    if (!file) {
        LOG_ERROR("Failed to open config file: %s", filepath);
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
            STR_COPY_FIELD(g_server.name, value);
            field_count++;
        } else if (field_count == 1) {
            // Port
            int port = atoi(value);
            if (port <= 0 || port > 65535) {
                LOG_ERROR("Invalid port: %s", value);
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
        LOG_ERROR("Incomplete config file. Expected 2 fields, got %d", field_count);
        return 0;
    }
    
    // Validation
    if (strlen(g_server.name) == 0) {
        LOG_ERROR("Server name cannot be empty");
        return 0;
    }
    
    if (g_server.port == 0) {
        LOG_ERROR("Port cannot be 0");
        return 0;
    }
    
    // Success - print loaded config
    LOG_INFO("=== Realm Server Configuration ===");
    LOG_INFO("Server Name: %s", g_server.name);
    LOG_INFO("Port: %d", g_server.port);
    LOG_INFO("===================================");
    
    return 1;  // Success
}
