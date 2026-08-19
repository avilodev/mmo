/**
 * @file
 * Run the realm service, authenticate clients, and monitor configured world servers.
 */
#include "types.h"
#include "log.h"
#include "session.h"
#include "packet_limiter.h"
#include "limit_profiles.h"
#include "net_notify.h"

#include "players_database.h"
#include "character_connect.h"
#include "config.h"
#include "utils.h"
#include "routes.h"
#include "world_connect.h"
#include "realm_world_auth.h"
#include "world_database_manager.h"
#include "class_stats.h"
#include "data_paths.h"

#include <signal.h>
#include <errno.h>
#include <poll.h>
#include <arpa/inet.h>
#include <limits.h>
#include <string.h>

#define WORLD_QUERY_TIMEOUT 15

static char g_world_file_path[PATH_MAX];

static int append_path(char* destination, size_t destination_size,
                       const char* base, const char* suffix) {
    size_t base_length = strlen(base);
    size_t suffix_length = strlen(suffix);

    if (base_length + suffix_length + 1 > destination_size) return 0;

    memcpy(destination, base, base_length);
    memcpy(destination + base_length, suffix, suffix_length + 1);
    return 1;
}

/**
 * Receive an exact byte count while applying a poll timeout to each read.
 *
 * @return      The requested byte count, or -1 on timeout, socket error, or disconnection.
 */
static ssize_t recv_exact_timeout(int fd, void* buffer, size_t length, int timeout_ms) {
    uint8_t* ptr = buffer;
    size_t total = 0;
    while (total < length) {
        struct pollfd wait_fd = {.fd = fd, .events = POLLIN};
        int ready = poll(&wait_fd, 1, timeout_ms);
        if (ready <= 0 || !(wait_fd.revents & POLLIN)) return -1;
        ssize_t got = recv(fd, ptr + total, length - total, 0);
        if (got <= 0) return -1;
        total += (size_t)got;
    }
    return (ssize_t)total;
}

/** Resolve the world-list path relative to the realm-server executable. */
static void init_runtime_paths(void) {
    char exe_path[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
    if (len <= 0) {
        snprintf(g_world_file_path, sizeof(g_world_file_path),
                 "realm_server/worlds/worlds.txt");
        return;
    }

    exe_path[len] = '\0';
    char* slash = strrchr(exe_path, '/');
    if (slash) *slash = '\0';
    if (!append_path(g_world_file_path, sizeof(g_world_file_path), exe_path,
                     "/../worlds/worlds.txt")) {
        fprintf(stderr, "Realm world-list path is too long\n");
        exit(EXIT_FAILURE);
    }
}

/**
 * Authenticate one realm client and route its reassembled post-authentication packets.
 *
 * The function owns and frees the heap-allocated descriptor argument, then closes the client descriptor before returning.
 *
 * @return      Always NULL.
 */
void* client_handler_thread(void* arg) {
    int client_fd = *(int*)arg;
    free(arg);

    printf("Realm client connected: fd %d\n", client_fd);

    // Fresh budget for this connection, before a single packet is read.
    packet_limiter_reset(client_fd);

    uint8_t buffer[MAX_PACKET_SIZE];
    ssize_t buf_len = 0;
    struct pollfd pfd = {.fd = client_fd, .events = POLLIN};

    int authenticated = 0;
    uint32_t account_id = 0;

    while (g_server.running) {
        int ret = poll(&pfd, 1, 1000);

        if (ret < 0) {
            if (errno == EINTR) continue;
            printf("Client fd %d: poll error %d\n", client_fd, errno);
            break;
        }
        if (ret == 0) continue;

        if (pfd.revents & POLLIN) {
            ssize_t bytes = recv(client_fd, buffer + buf_len,
                                 sizeof(buffer) - (size_t)buf_len, 0);

            if (bytes <= 0) {
                printf("Client fd %d disconnected (recv returned %zd)\n", client_fd, bytes);
                break;
            }
            buf_len += bytes;

            if (!authenticated) {
                printf("Client fd %d: Not authenticated yet, have %zd/%zu bytes\n",
                       client_fd, buf_len, sizeof(RealmConnectPacket));

                if (buf_len < (ssize_t)sizeof(RealmConnectPacket)) {
                    continue;  // Wait for rest of packet
                }

                {
                    RealmConnectPacket* pkt = (RealmConnectPacket*)buffer;

                    printf("Client fd %d: Packet type: %d (expected PACKET_REALM_CONNECT=%d)\n",
                           client_fd, pkt->header.type, PACKET_REALM_CONNECT);

                    if (pkt->header.type == PACKET_REALM_CONNECT) {
                        account_id = ntohl(pkt->header.player_id);

                        printf("Client fd %d: Player ID: %u\n", client_fd, account_id);
                        printf("Client fd %d: Validating session...\n", client_fd);

                        if (session_validate(account_id, pkt->header.session_key)) {
                            authenticated = 1;
                            buf_len = 0;  // Reset buffer for post-auth packets

                            printf("Client fd %d: Session validation SUCCESS!\n", client_fd);

                            RealmConnectAckPacket response = {0};
                            response.header.type = PACKET_REALM_CONNECT_ACK;
                            response.header.player_id = htonl(account_id);
                            response.header.payload_size = htons(sizeof(response) - sizeof(PacketHeader));
                            response.success = 1;
                            strncpy(response.message, "Welcome to Realm Server", 127);

                            ssize_t sent = send(client_fd, &response, sizeof(response), 0);
                            if (sent != (ssize_t)sizeof(response))
                                printf("Client fd %d: Warning: partial/failed auth ack send\n", client_fd);
                            else
                                printf("Client fd %d: Sent ack packet (%zd bytes)\n", client_fd, sent);
                            printf("Account %u authenticated on realm server\n", account_id);
                            continue;
                        } else {
                            printf("Client fd %d: Session validation FAILED!\n", client_fd);
                        }
                    } else {
                        printf("Client fd %d: Wrong packet type received\n", client_fd);
                    }
                }

                // Authentication failed
                printf("Client fd %d: Sending authentication failure response\n", client_fd);
                RealmConnectAckPacket response = {0};
                response.header.type = PACKET_REALM_CONNECT_ACK;
                response.header.payload_size = htons(sizeof(response) - sizeof(PacketHeader));
                response.success = 0;
                strncpy(response.message, "Invalid session", 127);
                if (send(client_fd, &response, sizeof(response), 0) != (ssize_t)sizeof(response))
                    printf("Client fd %d: Warning: partial/failed auth failure send\n", client_fd);
                printf("Client fd %d: Breaking connection due to auth failure\n", client_fd);
                break;
            }

            // Handle realm packets — drain all complete packets from buffer
            uint8_t* ptr = buffer;
            ssize_t remaining = buf_len;

            while (remaining >= (ssize_t)sizeof(PacketHeader)) {
                PacketHeader* hdr = (PacketHeader*)ptr;
                ssize_t pkt_size = (ssize_t)sizeof(PacketHeader) + (ssize_t)ntohs(hdr->payload_size);

                if (pkt_size > (ssize_t)MAX_PACKET_SIZE) {
                    printf("Client fd %d: oversized packet (%zd bytes), disconnecting\n", client_fd, pkt_size);
                    uint8_t dc[sizeof(DisconnectPacket)];
                    size_t dn = net_build_disconnect(dc, sizeof(dc),
                                                     DISCONNECT_REASON_PROTOCOL, NULL);
                    if (dn) send(client_fd, dc, dn, MSG_NOSIGNAL);
                    goto disconnect;
                }

                if (remaining < pkt_size)
                    break;  // incomplete — wait for more data

                printf("Client fd %d: Processing authenticated packet (%zd bytes)\n", client_fd, pkt_size);
                if (process_packet(client_fd, account_id, ptr, pkt_size) < 0)
                    goto disconnect;   // limiter asked for the socket to close

                ptr += pkt_size;
                remaining -= pkt_size;
            }

            if (remaining > 0 && ptr != buffer)
                memmove(buffer, ptr, (size_t)remaining);
            buf_len = remaining;
        }

        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            printf("Client fd %d: Socket error (revents: %d)\n", client_fd, pfd.revents);
            break;
        }
    }

disconnect:
    // Release the budget so a recycled descriptor never inherits it.
    packet_limiter_reset(client_fd);
    close(client_fd);
    printf("Realm client handler exiting for fd %d\n", client_fd);
    return NULL;
}

/**
 * Accept realm connections and detach one handler thread per client.
 *
 * @return      Always NULL after the server stops.
 */
void* accept_thread_func(void* arg) {
    (void)arg;

    struct pollfd pfd = {.fd = g_server.tcp_sockfd, .events = POLLIN};

    while (g_server.running) {
        int ret = poll(&pfd, 1, 1000);
        if (ret <= 0) continue;

        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);

        int client_fd = accept(g_server.tcp_sockfd, (struct sockaddr*)&client_addr, &addr_len);

        if (client_fd < 0) continue;

        printf("New realm connection from %s:%d\n", inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));

        pthread_t thread;
        int* client_fd_ptr = malloc(sizeof(int));
        *client_fd_ptr = client_fd;

        if (pthread_create(&thread, NULL, client_handler_thread, client_fd_ptr) != 0) {
            close(client_fd);
            free(client_fd_ptr);
            continue;
        }

        pthread_detach(thread);
    }

    return NULL;
}

/**
 * Maintain authenticated world connections and update their heartbeat status.
 *
 * This function holds world_servers_lock while connecting to and querying every configured world.
 *
 * @return      Always NULL after monitoring stops or world configuration cannot be loaded.
 */
void* world_monitor_thread_func(void* arg) {
    (void)arg;

    pthread_mutex_lock(&g_server.world_servers_lock);

    g_server.num_world_servers = load_world_servers_from_file(
        g_world_file_path,
        g_server.world_servers,
        MAX_WORLDS
    );

    if (g_server.num_world_servers < 0) {
        printf("Failed to load world servers from config file\n");
        pthread_mutex_unlock(&g_server.world_servers_lock);
        return NULL;
    }

    if (g_server.num_world_servers == 0) {
        printf("No world servers found in config file\n");
        pthread_mutex_unlock(&g_server.world_servers_lock);
        return NULL;
    }

    pthread_mutex_unlock(&g_server.world_servers_lock);

    printf("World monitor thread started\n");

    while (g_server.running) {
        time_t now = time(NULL);

        // Get today's server auth key from Redis
        char* server_key = get_server_auth_key_from_redis("global");
        if (!server_key) {
            printf("WARNING: No server auth key in Redis, retrying in 30s\n");
            sleep(WORLD_QUERY_TIMEOUT);
            continue;
        }

        // Probe every world against a private snapshot, with the lock RELEASED.
        //
        // This loop performs blocking network I/O: a connect per offline world
        // and, per online world, a 5s poll plus a 5s receive. Holding
        // world_servers_lock across all of that stalled every client that
        // wanted to list worlds or enter one, because those paths take the same
        // lock -- so one unresponsive world delayed everybody's login by up to
        // its full timeout, over and over. Only this thread ever mutates fd,
        // and the shutdown sweep below runs after this loop exits on this same
        // thread, so the snapshot cannot race another writer.
        WorldServer probe[MAX_WORLDS];
        int probe_count;

        pthread_mutex_lock(&g_server.world_servers_lock);
        probe_count = g_server.num_world_servers;
        if (probe_count > MAX_WORLDS) probe_count = MAX_WORLDS;
        memcpy(probe, g_server.world_servers,
               sizeof(WorldServer) * (size_t)probe_count);
        pthread_mutex_unlock(&g_server.world_servers_lock);

        for (int i = 0; i < probe_count; i++) {
            WorldServer* ws = &probe[i];

            // Try to connect if not connected (throttle reconnect attempts)
            if (ws->fd < 0) {
                if (now - ws->last_heartbeat >= WORLD_QUERY_TIMEOUT) {
                    char* world_key = get_server_auth_key_from_redis(ws->name);
                    const char* key_to_use = world_key ? world_key : server_key;

                    // Use silent=1 to suppress spam, but track first failure
                    ws->fd = connect_to_world_server(ws->host, ws->port, key_to_use, 1);
                    ws->last_heartbeat = now;

                    if (world_key) free(world_key);

                    if (ws->fd < 0) {
                        if (!ws->connection_logged) {
                            printf("World server '%s' is offline - will retry silently every 30s\n", ws->name);
                            ws->connection_logged = 1;
                        }
                        ws->online = 0;
                        ws->player_count = 0;  // Reset player count when offline
                    } else {
                        printf("Successfully connected to world server '%s'\n", ws->name);
                        ws->online = 1;
                        ws->connection_logged = 0;  // Reset for next disconnect
                    }
                }
                continue;
            }

            // Send heartbeat
            WorldHeartbeatPacket hb = {0};
            hb.header.type = PACKET_WORLD_HEARTBEAT;
            hb.header.player_id = 0;
            hb.header.payload_size = 0;
            hb.timestamp = now;

            if (send(ws->fd, &hb, sizeof(hb), 0) <= 0) {
                //printf("World server %s disconnected (send failed)\n", ws->name);
                close(ws->fd);
                ws->fd = -1;
                ws->online = 0;
                ws->last_heartbeat = now;
                continue;
            }

            // Read response with timeout
            WorldStatusPacket status;
            struct pollfd pfd = {.fd = ws->fd, .events = POLLIN};
            int poll_ret = poll(&pfd, 1, 5000); // 5 second timeout

            if (poll_ret <= 0) {
                //printf("World server %s heartbeat timeout\n", ws->name);
                close(ws->fd);
                ws->fd = -1;
                ws->online = 0;
                ws->last_heartbeat = now;
                continue;
            }

            ssize_t recv_ret = recv_exact_timeout(ws->fd, &status, sizeof(status), 5000);
            if (recv_ret <= 0) {
                //printf("World server %s disconnected (recv failed)\n", ws->name);
                close(ws->fd);
                ws->fd = -1;
                ws->online = 0;
                ws->last_heartbeat = now;
                continue;
            }

            if (status.header.type == PACKET_WORLD_STATUS) {
                ws->player_count = ntohl(status.player_count);
                ws->max_players = ntohl(status.max_players);
                ws->online = (status.status == 1);
                ws->last_heartbeat = now;

                printf("[%s] Status: %u/%u players, uptime: %lu seconds\n", ws->name, ws->player_count, ws->max_players, (unsigned long)status.uptime);
            }
        }

        // Publish the probe results. Name, host and port come from config and
        // are never mutated here, so only live status is written back.
        pthread_mutex_lock(&g_server.world_servers_lock);
        for (int i = 0; i < probe_count; i++) {
            WorldServer* dst = &g_server.world_servers[i];
            dst->fd                = probe[i].fd;
            dst->online            = probe[i].online;
            dst->player_count      = probe[i].player_count;
            dst->max_players       = probe[i].max_players;
            dst->last_heartbeat    = probe[i].last_heartbeat;
            dst->connection_logged = probe[i].connection_logged;
        }
        pthread_mutex_unlock(&g_server.world_servers_lock);

        free(server_key);

        // Sleep 10 seconds between heartbeat cycles
        sleep(10);
    }

    // Cleanup connections
    pthread_mutex_lock(&g_server.world_servers_lock);
    for (int i = 0; i < g_server.num_world_servers; i++) {
        if (g_server.world_servers[i].fd >= 0) {
            close(g_server.world_servers[i].fd);
        }
    }
    pthread_mutex_unlock(&g_server.world_servers_lock);

    printf("World monitor thread exiting\n");
    return NULL;
}

/**
 * Initialize realm dependencies and run client and world-monitor threads until shutdown.
 *
 * @param argc  Argument count; an optional first argument names the configuration file.
 * @param argv  Argument vector containing the optional configuration path.
 * @return      Zero after orderly shutdown, or one when initialization fails.
 */
int main(int argc, char** argv) {
    log_init();   // reads MMO_LOG_LEVEL; must run before any thread starts

    printf("=== REALM SERVER ===\n");
    printf("PID: %d\n", getpid());

    // Sizes its slot table from RLIMIT_NOFILE, so it must run after any
    // descriptor limit changes and before the accept loop starts.
    packet_limiter_init(limit_profile_realm());

    init_runtime_paths();

    /* The realm server owns character creation, so it needs the race registry to
     * validate what a client asks to create. Without it every create is refused,
     * which is the correct failure for a missing registry but a confusing one, so
     * say plainly what happened. */
    char races_path[512], progression_path[512];
    data_path_resolve(races_path, sizeof(races_path), "/data/races.json");
    data_path_resolve(progression_path, sizeof(progression_path), "/data/progression.json");
    if (!class_stats_init(races_path, progression_path)) {
        fprintf(stderr, "Failed to load the race registry from %s — "
                        "character creation will refuse every request\n", races_path);
    }

    if (!session_init()) {
        fprintf(stderr, "Failed to initialize Redis session connection\n");
        return 1;
    }

    char* key = get_server_auth_key_from_redis("global");
    if (!key) {
        set_server_auth_key_in_redis("global", "default_server_secret_123", 0);
    }
    else free(key);

    memset(&g_server, 0, sizeof(g_server));
    memset(&g_state, 0, sizeof(g_state));
    g_server.running = 1;  // Initialize running state
    pthread_mutex_init(&g_server.world_servers_lock, NULL);

    // Load config if provided
    if (argc > 1) {
        if (!set_config(argv[1])) {
            printf("Usage: ./realm_server <config_file.conf>\n");
            exit(1);
        }
    } else {
        // Use defaults
        g_server.port = REALM_SERVER_PORT;
        strncpy(g_server.name, "Default-Realm", sizeof(g_server.name) - 1);
    }

    setup_signals();

    g_server.tcp_sockfd = create_tcp_server_socket(g_server.port);
    if (g_server.tcp_sockfd < 0) {
        character_database_close();
        session_close();
        return 1;
    }

    if (pthread_create(&g_server.accept_thread, NULL, accept_thread_func, NULL) != 0) {
        close(g_server.tcp_sockfd);
        character_database_close();
        session_close();
        return 1;
    }

    // Start world server monitoring thread
    if (pthread_create(&g_server.world_monitor_thread, NULL, world_monitor_thread_func, NULL) != 0) {
        fprintf(stderr, "Failed to create world monitor thread\n");
        g_server.running = 0;
        pthread_join(g_server.accept_thread, NULL);
        close(g_server.tcp_sockfd);
        character_database_close();
        session_close();
        return 1;
    }

    printf("Realm Server '%s' running on port %d\n", g_server.name, g_server.port);

    while (g_server.running) {
        sleep(1);
    }

    printf("\nShutting down...\n");
    close(g_server.tcp_sockfd);
    pthread_join(g_server.accept_thread, NULL);
    pthread_join(g_server.world_monitor_thread, NULL);
    world_databases_cleanup();
    session_close();
    pthread_mutex_destroy(&g_server.world_servers_lock);

    printf("Realm Server stopped\n");
    return 0;
}
