/**
 * @file
 * Load world-server identity, endpoint, capacity, and packet-budget settings.
 */
#include "config.h"
#include "log.h"
#include "str_fixed.h"

#include <errno.h>
#include <string.h>

ServerConfig g_server;
ServerState g_state;

/**
 * Create, bind, and listen on a reusable IPv4 TCP socket.
 *
 * @return      The listening descriptor, or -1 when setup fails.
 */
int create_tcp_server_socket(int port) {
    return create_bound_server_socket(NULL, port);
}

/**
 * Create, bind, and listen on a reusable IPv4 TCP socket at one address.
 *
 * @param bind_addr  Dotted-quad address, or NULL/"" for every interface.
 * @return           The listening descriptor, or -1 when setup fails.
 */
int create_bound_server_socket(const char* bind_addr, int port) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        LOG_ERROR("Socket creation failed for port %d: %s", port, strerror(errno));
        return -1;
    }

    int opt = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        LOG_ERROR("setsockopt SO_REUSEADDR failed: %s", strerror(errno));
        close(sock);
        return -1;
    }

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);

    if (bind_addr && *bind_addr) {
        if (inet_pton(AF_INET, bind_addr, &server_addr.sin_addr) != 1) {
            LOG_ERROR("'%s' is not an address this listener can bind to", bind_addr);
            close(sock);
            return -1;
        }
    } else {
        server_addr.sin_addr.s_addr = INADDR_ANY;
    }

    if (bind(sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        LOG_ERROR("Bind to %s:%d failed: %s",
                  (bind_addr && *bind_addr) ? bind_addr : "0.0.0.0",
                  port, strerror(errno));
        close(sock);
        return -1;
    }

    if (listen(sock, MAX_PENDING_CONNECTIONS) < 0) {
        LOG_ERROR("Listen on port %d failed: %s", port, strerror(errno));
        close(sock);
        return -1;
    }

    LOG_INFO("TCP server socket bound to %s:%d",
             (bind_addr && *bind_addr) ? bind_addr : "0.0.0.0", port);
    return sock;
}

// Strip trailing whitespace in place.
static void rtrim(char* s) {
    size_t n = strlen(s);
    while (n > 0 && isspace((unsigned char)s[n - 1])) s[--n] = '\0';
}

/**
 * Apply one named world setting to the global world configuration.
 *
 * @return      Nonzero when the key is recognized, otherwise zero.
 */
/** Read a boolean-ish setting: on/true/yes/1 are true, everything else false. */
static int parse_bool(const char* value) {
    return strcmp(value, "1") == 0 || strcasecmp(value, "on") == 0 ||
           strcasecmp(value, "true") == 0 || strcasecmp(value, "yes") == 0;
}

static int apply_keyed_setting(const char* key, const char* value) {
    /* Answer the online-roster request. Off unless a deployment asks for it. */
    if (strcmp(key, "session_list") == 0) {
        g_server.session_list_enabled = parse_bool(value);
        if (g_server.session_list_enabled) {
            LOG_WARN("session_list = on: any authenticated client can enumerate "
                     "every online player's name, level and ping");
        }
        return 1;
    }

    /* Port the realm's heartbeat link connects to. Normally left unset and
     * taken from worlds.conf; here for a deployment that has to move it. */
    if (strcmp(key, "realm_port") == 0) {
        int port = atoi(value);
        if (port > 0 && port <= 65535) {
            g_server.realm_port = (uint16_t)port;
        } else {
            LOG_ERROR("realm_port = %s is out of range; keeping %u",
                      value, (unsigned)g_server.realm_port);
        }
        return 1;
    }

    /* Interface the realm listener binds to. Empty means all of them. */
    if (strcmp(key, "realm_bind") == 0) {
        snprintf(g_server.realm_bind, sizeof(g_server.realm_bind), "%s", value);
        return 1;
    }

    // How many NPCs this world's pool can hold. See npc_world.h.
    if (strcmp(key, "max_npcs") == 0) {
        g_server.max_npcs = atoi(value);
        return 1;
    }

    /* Repeatable: each line adds one source to the realm-handshake allowlist.
     * A rule the operator mistyped is refused rather than stored as something
     * wider than intended, because the failure mode of a wrong allowlist is a
     * world port that accepts realm handshakes from strangers. */
    if (strcmp(key, "realm_allow") == 0) {
        if (!ip_allowlist_add(&g_server.realm_allow, value))
            LOG_ERROR("Invalid realm_allow rule '%s' (ignored)", value);
        return 1;
    }

    if (strcmp(key, "realm_max_handlers") == 0) {
        int requested = atoi(value);
        if (requested < 1 || requested > REALM_MAX_HANDLERS_LIMIT) {
            LOG_ERROR("realm_max_handlers is %d but must be 1..%d; keeping %d",
                      requested, REALM_MAX_HANDLERS_LIMIT, g_server.realm_max_handlers);
        } else {
            g_server.realm_max_handlers = requested;
        }
        return 1;
    }

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
        LOG_ERROR("Failed to open config file: %s", filepath);
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
    g_server.max_npcs = 0;   // 0 means "use the compiled default"; see npc_world.h
    memset(&g_server.limits, 0, sizeof(g_server.limits));
    ip_allowlist_reset(&g_server.realm_allow);
    g_server.realm_max_handlers = REALM_MAX_HANDLERS_DEFAULT;

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
                LOG_ERROR("Unknown config key '%s' in %s (ignored)",
                          key, filepath);
            continue;
        }

        // Parse based on field order
        if (field_count == 0) {
            // Server Name
            STR_COPY_FIELD(g_server.server_name, value);
            field_count++;
        } else if (field_count == 1) {
            // Region
            STR_COPY_FIELD(g_server.region, value);
            field_count++;
        } else if (field_count == 2) {
            // IP:Port
            char ip[16];
            int port;
            if (sscanf(value, "%15[^:]:%d", ip, &port) == 2) {
                STR_COPY_FIELD(g_server.ip, ip);
                g_server.port = port;
                field_count++;
            } else {
                LOG_ERROR("Invalid IP:Port format: %s", value);
                fclose(file);
                return 0;
            }
        } else if (field_count == 3) {
            /* Max Players.
             *
             * Checked against both ceilings that stand behind this field, because
             * exceeding either one used to be silent:
             *
             *   MAX_PLAYERS  -- active_players[] and every array beside it are
             *     compiled at this size. A larger configured value loaded fine,
             *     printed itself in the startup banner, and then made
             *     player_add_active() log "no available slots!" once per rejected
             *     login forever, naming neither number. Worse, the value is sent
             *     to the realm server in the status heartbeat (main.c) and the
             *     realm flags a world full at it (realm_server world_list.c), so
             *     an over-large value also stops the world from ever being marked
             *     full while it is in fact refusing logins.
             *
             *   UINT16_MAX   -- the field is uint16_t. Parsing into an int and
             *     assigning meant 70000 became 4464 with no diagnostic.
             *
             * Refusing to start is the right failure: the operator asked for a
             * capacity this build cannot provide, and quietly providing a
             * different one is how both bugs above happened.
             */
            long max_players = strtol(value, NULL, 10);
            if (max_players <= 0) {
                LOG_ERROR("Invalid max_players: %s", value);
                fclose(file);
                return 0;
            }
            if (max_players > MAX_PLAYERS) {
                LOG_ERROR("max_players is %ld but this build supports at most %d "
                          "(MAX_PLAYERS in common/include/types.h). Lower the value "
                          "in the world .conf, or raise MAX_PLAYERS and rebuild.",
                          max_players, MAX_PLAYERS);
                fclose(file);
                return 0;
            }
            if (max_players > UINT16_MAX) {
                LOG_ERROR("max_players is %ld but the field holds at most %u.",
                          max_players, (unsigned)UINT16_MAX);
                fclose(file);
                return 0;
            }
            g_server.max_players = (uint16_t)max_players;
            field_count++;
        } else if (field_count == 4) {
            // Hardcore
            int hardcore = atoi(value);
            if (hardcore != 0 && hardcore != 1) {
                LOG_ERROR("Invalid hardcore value (must be 0 or 1): %s", value);
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
        LOG_ERROR("Incomplete config file. Expected 5 fields, got %d", field_count);
        return 0;
    }
    
    // Validation
    if (strlen(g_server.server_name) == 0) {
        LOG_ERROR("Server name cannot be empty");
        return 0;
    }
    
    if (strlen(g_server.region) == 0) {
        LOG_ERROR("Region cannot be empty");
        return 0;
    }
    
    if (strlen(g_server.ip) == 0) {
        LOG_ERROR("IP cannot be empty");
        return 0;
    }
    
    if (g_server.port == 0) {
        LOG_ERROR("Port cannot be 0");
        return 0;
    }
    
    if (g_server.max_players == 0) {
        LOG_ERROR("Max players cannot be 0");
        return 0;
    }
    
    /* No realm_allow lines means "this host", not "anywhere".
     *
     * The realm handshake is recognised by the first byte on the shared player
     * listener, so an empty allowlist would leave it reachable from every
     * address that can reach the game port.
     *
     * The default is loopback plus this world's OWN configured address. The
     * second half is not redundant: when worlds.txt names a routable address so
     * remote clients can reach the world -- which is what any deployment past a
     * single machine has to do -- the realm connects to that address, and the
     * kernel then picks the matching interface address as the source rather
     * than 127.0.0.1. A loopback-only default would refuse the realm's own
     * handshake and take every world offline in the world list, with nothing to
     * suggest an allowlist was the cause.
     *
     * It cannot widen access: the only address added is one this host already
     * answers on. Anything genuinely remote still has to be listed. */
    if (ip_allowlist_is_empty(&g_server.realm_allow)) {
        ip_allowlist_add(&g_server.realm_allow, "127.0.0.0/8");
        ip_allowlist_add(&g_server.realm_allow, "::1/128");
        if (g_server.ip[0])
            ip_allowlist_add(&g_server.realm_allow, g_server.ip);
    }

    char allow_text[256];
    ip_allowlist_describe(&g_server.realm_allow, allow_text, sizeof(allow_text));

    // Success - print loaded config
    LOG_INFO("=== Server Configuration ===");
    LOG_INFO("Server Name: %s", g_server.server_name);
    LOG_INFO("Region: %s", g_server.region);
    LOG_INFO("IP:Port: %s:%u", g_server.ip, g_server.port);
    LOG_INFO("Max Players: %u", g_server.max_players);
    LOG_INFO("Hardcore: %s", g_server.hardcore ? "Yes" : "No");
    LOG_INFO("Realm allowlist: %s", allow_text);
    LOG_INFO("Realm handler cap: %d", g_server.realm_max_handlers);
    LOG_INFO("===========================");

    g_state.current_players = 0;
    g_server.running = 1;
    
    return 1;  // Success
}
