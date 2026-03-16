#include "types.h"
#include "session.h"

#include "players_database.h"
#include "character_connect.h"
#include "config.h"
#include "utils.h"
#include "routes.h"
#include "world_connect.h"
#include "realm_world_auth.h" 
#include "world_database_manager.h"

#include <signal.h>
#include <errno.h>
#include <poll.h>
#include <arpa/inet.h>

#define WORLD_QUERY_TIMEOUT 15

void* client_handler_thread(void* arg) {
    int client_fd = *(int*)arg;
    free(arg);

    printf("Realm client connected: fd %d\n", client_fd);
    
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
                        printf("Client fd %d: Session key: %.32s\n", client_fd, pkt->header.session_key);
                        
                        printf("Client fd %d: Validating session...\n", client_fd);
                        
                        if (session_validate(account_id, pkt->header.session_key)) {
                            authenticated = 1;
                            buf_len = 0;  // Reset buffer for post-auth packets

                            printf("Client fd %d: Session validation SUCCESS!\n", client_fd);

                            RealmConnectAckPacket response = {0};
                            response.header.type = PACKET_REALM_CONNECT_ACK;
                            response.header.player_id = htonl(account_id);
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
                response.success = 0;
                strncpy(response.message, "Invalid session", 127);
                if (send(client_fd, &response, sizeof(response), 0) != (ssize_t)sizeof(response))
                    printf("Client fd %d: Warning: partial/failed auth failure send\n", client_fd);
                printf("Client fd %d: Breaking connection due to auth failure\n", client_fd);
                break;
            }
            
            // Handle realm packets
            printf("Client fd %d: Processing authenticated packet (%zd bytes)\n", client_fd, buf_len);
            process_packet(client_fd, account_id, buffer, buf_len);
            buf_len = 0;
        }
        
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            printf("Client fd %d: Socket error (revents: %d)\n", client_fd, pfd.revents);
            break;
        }
    }
    
    close(client_fd);
    printf("Realm client handler exiting for fd %d\n", client_fd);
    return NULL;
}

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

// World server monitoring thread
void* world_monitor_thread_func(void* arg) {
    (void)arg;
    
    pthread_mutex_lock(&g_server.world_servers_lock);
    
    g_server.num_world_servers = load_world_servers_from_file(
        WORLD_FILE_PATH, 
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
        
        pthread_mutex_lock(&g_server.world_servers_lock);
        
        for (int i = 0; i < g_server.num_world_servers; i++) {
            WorldServer* ws = &g_server.world_servers[i];
            
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
            
            ssize_t recv_ret = recv(ws->fd, &status, sizeof(status), 0);
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

int main(int argc, char** argv) {
    printf("=== REALM SERVER ===\n");
    printf("PID: %d\n", getpid());
    
    session_init();

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
        return 1;
    }
    
    if (pthread_create(&g_server.accept_thread, NULL, accept_thread_func, NULL) != 0) {
        close(g_server.tcp_sockfd);
        character_database_close();
        return 1;
    }
    
    // Start world server monitoring thread
    if (pthread_create(&g_server.world_monitor_thread, NULL, world_monitor_thread_func, NULL) != 0) {
        fprintf(stderr, "Failed to create world monitor thread\n");
        g_server.running = 0;
        pthread_join(g_server.accept_thread, NULL);
        close(g_server.tcp_sockfd);
        character_database_close();
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
    pthread_mutex_destroy(&g_server.world_servers_lock);
    
    printf("Realm Server stopped\n");
    return 0;
}