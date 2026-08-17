/**
 * @file
 * Load world-server identity, endpoint, capacity, and packet-budget settings.
 */
#include "config.h"

ServerConfig g_server;
ServerState g_state;

/**
 * Create, bind, and listen on a reusable IPv4 TCP socket.
 *
 * @return      The listening descriptor, or -1 when setup fails.
 */
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

// Strip trailing whitespace in place.
static void rtrim(char* s) {
    size_t n = strlen(s);
    while (n > 0 && isspace((unsigned char)s[n - 1])) s[--n] = '\0';
}

/**
 * Apply one named packet-limit override to the global world configuration.
 *
 * @return      Nonzero when the key is recognized, otherwise zero.
 */
static int apply_keyed_setting(const char* key, const char* value) {
    // limit_overall_rate / limit_overall_burst
    if (strcmp(key, "limit_overall_rate") == 0) {
        g_server.limits.overall_rate = atof(value);
        return 1;
    }
    if (strcmp(key, "limit_overall_burst") == 0) {
        g_server.limits.overall_capacity = atof(value);
        return 1;
    }
    if (strcmp(key, "limit_violation_limit") == 0) {
        g_server.limits.violation_limit = (uint32_t)atoi(value);
        return 1;
    }
    if (strcmp(key, "limit_violation_window") == 0) {
        g_server.limits.violation_window = atof(value);
        return 1;
    }

    // limit_<class>_rate / limit_<class>_burst
    if (strncmp(key, "limit_", 6) == 0) {
        const char* rest = key + 6;
        const char* suffix = strrchr(rest, '_');
        if (suffix) {
            char class_name[32] = {0};
            size_t len = (size_t)(suffix - rest);
            if (len < sizeof(class_name)) {
                memcpy(class_name, rest, len);
                int cls = packet_limiter_class_from_name(class_name);
                if (cls >= 0) {
                    if (strcmp(suffix, "_rate") == 0) {
                        g_server.limits.class_rate[cls] = atof(value);
                        return 1;
                    }
                    if (strcmp(suffix, "_burst") == 0) {
                        g_server.limits.class_capacity[cls] = atof(value);
                        return 1;
                    }
                }
            }
        }
    }

    return 0;
}

/**
 * Load five positional world fields followed by optional named limit overrides.
 *
 * @return      Nonzero when all required fields are valid, otherwise zero.
 */
int set_config(const char* filepath) {
    FILE* file = fopen(filepath, "r");
    if (!file) {
        fprintf(stderr, "Failed to open config file: %s\n", filepath);
        return 0;
    }

    char line[256];
    int field_count = 0;

    // Initialize defaults
    memset(g_server.server_name, 0, sizeof(g_server.server_name));
    memset(g_server.region, 0, sizeof(g_server.region));
    memset(g_server.ip, 0, sizeof(g_server.ip));
    g_server.port = 0;
    g_server.max_players = 0;
    g_server.hardcore = false;
    memset(&g_server.limits, 0, sizeof(g_server.limits));

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

        // A line containing '=' is a named setting. Everything else falls
        // through to the original positional parsing, so config files written
        // before named settings existed still load exactly as they did.
        char* eq = strchr(value, '=');
        if (eq) {
            *eq = '\0';
            char* key = value;
            char* val = eq + 1;

            rtrim(key);
            while (*val && isspace((unsigned char)*val)) val++;
            rtrim(val);

            if (!apply_keyed_setting(key, val))
                fprintf(stderr, "Unknown config key '%s' in %s (ignored)\n",
                        key, filepath);
            continue;
        }

        // Parse based on field order
        if (field_count == 0) {
            // Server Name
            strncpy(g_server.server_name, value, sizeof(g_server.server_name) - 1);
            field_count++;
        } else if (field_count == 1) {
            // Region
            strncpy(g_server.region, value, sizeof(g_server.region) - 1);
            field_count++;
        } else if (field_count == 2) {
            // IP:Port
            char ip[16];
            int port;
            if (sscanf(value, "%15[^:]:%d", ip, &port) == 2) {
                strncpy(g_server.ip, ip, sizeof(g_server.ip) - 1);
                g_server.port = port;
                field_count++;
            } else {
                fprintf(stderr, "Invalid IP:Port format: %s\n", value);
                fclose(file);
                return 0;
            }
        } else if (field_count == 3) {
            // Max Players
            int max_players = atoi(value);
            if (max_players <= 0) {
                fprintf(stderr, "Invalid max_players: %s\n", value);
                fclose(file);
                return 0;
            }
            g_server.max_players = max_players;
            field_count++;
        } else if (field_count == 4) {
            // Hardcore
            int hardcore = atoi(value);
            if (hardcore != 0 && hardcore != 1) {
                fprintf(stderr, "Invalid hardcore value (must be 0 or 1): %s\n", value);
                fclose(file);
                return 0;
            }
            g_server.hardcore = (hardcore == 1);
            field_count++;
            // Deliberately does not break: named settings may follow the
            // positional block, and stopping here would silently ignore them.
        }
    }
    
    fclose(file);
    
    // Verify all positional fields were read
    if (field_count != 5) {
        fprintf(stderr, "Incomplete config file. Expected 5 fields, got %d\n", field_count);
        return 0;
    }
    
    // Validation
    if (strlen(g_server.server_name) == 0) {
        fprintf(stderr, "Server name cannot be empty\n");
        return 0;
    }
    
    if (strlen(g_server.region) == 0) {
        fprintf(stderr, "Region cannot be empty\n");
        return 0;
    }
    
    if (strlen(g_server.ip) == 0) {
        fprintf(stderr, "IP cannot be empty\n");
        return 0;
    }
    
    if (g_server.port == 0) {
        fprintf(stderr, "Port cannot be 0\n");
        return 0;
    }
    
    if (g_server.max_players == 0) {
        fprintf(stderr, "Max players cannot be 0\n");
        return 0;
    }
    
    // Success - print loaded config
    printf("=== Server Configuration ===\n");
    printf("Server Name: %s\n", g_server.server_name);
    printf("Region: %s\n", g_server.region);
    printf("IP:Port: %s:%u\n", g_server.ip, g_server.port);
    printf("Max Players: %u\n", g_server.max_players);
    printf("Hardcore: %s\n", g_server.hardcore ? "Yes" : "No");
    printf("===========================\n");

    g_state.current_players = 0;
    g_server.running = 1;
    
    return 1;  // Success
}
