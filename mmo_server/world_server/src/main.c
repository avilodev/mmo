#include "types.h"
#include "session.h"
#include "realm_world_auth.h"
#include "world_database_config.h"

#include "ability_def.h"
#include "ability_handler.h"
#include "class_stats.h"
#include "config.h"
#include "combat.h"
#include "dialogue_system.h"
#include "projectile.h"
#include "loot.h"
#include "npc_ai.h"
#include "npc_spawns.h"
#include "routes.h"
#include "packet_handler.h"
#include "player_data.h"
#include "players_database.h"
#include "session_registry.h"
#include "party.h"
#include "quest_system.h"
#include "shop.h"
#include "utils.h"

#include <sys/socket.h>
#include <arpa/inet.h>
#include <signal.h>
#include <errno.h>
#include <poll.h>
#include <stddef.h>

// Data paths — computed at startup relative to the server binary (#11)
static char DATA_PATH[512];
static char ABILITIES_PATH[512];
static char DIALOGUES_PATH[512];
static char NPC_TYPES_PATH[512];
static char SPAWNS_PATH[512];
static char ATTACK_PROFILES_PATH[512];
static char QUESTS_PATH[512];
static char SHOPS_PATH[512];
static char QUEST_SAVE_DIR[512];

static void init_data_paths(void) {
    char exe[512] = {0};
    ssize_t len = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (len > 0) {
        exe[len] = '\0';
        char* slash = strrchr(exe, '/');
        if (slash) *slash = '\0';
    } else {
        exe[0] = '.';
        exe[1] = '\0';
    }
    snprintf(DATA_PATH,             sizeof(DATA_PATH),             "%s/data/items.json",            exe);
    snprintf(ABILITIES_PATH,        sizeof(ABILITIES_PATH),        "%s/data/abilities.json",        exe);
    snprintf(DIALOGUES_PATH,        sizeof(DIALOGUES_PATH),        "%s/data/dialogues",             exe);
    snprintf(NPC_TYPES_PATH,        sizeof(NPC_TYPES_PATH),        "%s/data/npc_types.json",        exe);
    snprintf(SPAWNS_PATH,           sizeof(SPAWNS_PATH),           "%s/data/spawns.json",           exe);
    snprintf(ATTACK_PROFILES_PATH,  sizeof(ATTACK_PROFILES_PATH),  "%s/data/attack_profiles.json",  exe);
    snprintf(QUESTS_PATH,           sizeof(QUESTS_PATH),           "%s/data/quests.json",           exe);
    snprintf(SHOPS_PATH,            sizeof(SHOPS_PATH),            "%s/data/shops.json",            exe);
    snprintf(QUEST_SAVE_DIR,        sizeof(QUEST_SAVE_DIR),        "%s/data/quests",                exe);
    printf("[PATHS] Data directory: %s/data/\n", exe);
}

static pthread_t g_combat_thread;
static pthread_t g_player_broadcast_thread;
static pthread_t g_npc_broadcast_thread;
static pthread_t g_projectile_broadcast_thread;

static volatile int g_combat_running = 0;
static volatile int g_player_broadcast_running = 0;
static volatile int g_npc_broadcast_running = 0;
static volatile int g_projectile_broadcast_running = 0;

 
// Global for tracking world server uptime
time_t g_server_start_time = 0; 

NPCWorld g_npc_world;

void* client_handler_thread(void* arg) { 
    int client_fd = *(int*)arg;
    free(arg);

    printf("World client handler started: fd %d\n", client_fd);
    
    uint8_t buffer[MAX_PACKET_SIZE * 2];  // Reassembly buffer (room for carry-over + new data)
    ssize_t buf_len = 0;                  // Bytes currently in buffer
    struct pollfd pfd = {.fd = client_fd, .events = POLLIN};

    int authenticated = 0;
    uint32_t character_id = 0;
    uint32_t account_id = 0;

#define PING_TIMEOUT_SECS 35   // ~3 missed 10s pings before server drops the connection
    time_t last_recv_time = time(NULL);

    while (g_server.running) {
        int ret = poll(&pfd, 1, 1000);

        if (ret < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (ret == 0) {
            // Check for ping timeout on authenticated players
            if (authenticated && (time(NULL) - last_recv_time) > PING_TIMEOUT_SECS) {
                printf("[TIMEOUT] Character %u timed out (no data for %ds)\n",
                       character_id, PING_TIMEOUT_SECS);
                break;
            }
            continue;
        }

        if (pfd.revents & POLLIN) {
            ssize_t bytes = recv(client_fd, buffer + buf_len,
                                 sizeof(buffer) - (size_t)buf_len, 0);

            if (bytes <= 0) break;
            last_recv_time = time(NULL);
            buf_len += bytes;

            if (!authenticated) {
                // Handle authentication
                if (buf_len >= (ssize_t)sizeof(WorldConnectPacket)) {
                    WorldConnectPacket* pkt = (WorldConnectPacket*)buffer;
                    
                    if (pkt->header.type == PACKET_WORLD_CONNECT) {
                        uint32_t world_id;
                        // Validate ticket and get account_id, character_id, world_id all at once
                        if (validate_game_ticket(pkt->game_ticket, &account_id, &character_id, &world_id)) {
                            // VERIFY OWNERSHIP FIRST (before adding to registry)
                            uint32_t owner = character_get_owner(character_id);
                            if (owner != account_id) {
                                printf("Character %u doesn't belong to account %u (owner=%u)!\n",
                                    character_id, account_id, owner);
                                
                                WorldConnectAckPacket response = {0};
                                response.header.type = PACKET_WORLD_CONNECT_ACK;
                                response.header.payload_size = htons(sizeof(response) - sizeof(PacketHeader));
                                response.success = 0;
                                strncpy(response.welcome_message,
                                    "Character ownership verification failed", 127);
                                send(client_fd, &response, sizeof(response), 0);
                                break;  // Exit without adding to registry
                            }

                            // PREVENT DUAL-LOGIN (only add to registry if ownership is valid)
                            if (session_registry_add(client_fd, account_id, character_id) != 0) {
                                printf("Account %u already logged in\n", account_id);
                                
                                WorldConnectAckPacket response = {0};
                                response.header.type = PACKET_WORLD_CONNECT_ACK;
                                response.header.payload_size = htons(sizeof(response) - sizeof(PacketHeader));
                                response.success = 0;
                                strncpy(response.welcome_message,
                                    "Account already logged in", 127);
                                send(client_fd, &response, sizeof(response), 0);
                                break;
                            }
                            
                            // Load character data
                            if (player_add_active(character_id, client_fd)) {
                                authenticated = 1;
                                g_state.current_players++;
                                
                                WorldConnectAckPacket response = {0};
                                response.header.type = PACKET_WORLD_CONNECT_ACK;
                                response.header.player_id = htonl(character_id);
                                response.header.payload_size = htons(sizeof(response) - sizeof(PacketHeader));
                                response.success = 1;
                                strncpy(response.welcome_message,
                                    "Welcome to the world!", 127);

                                send(client_fd, &response, sizeof(response), 0);
                                player_send_data_response(client_fd, character_id);

                                // Send stats once explicitly (no longer hidden inside data response)
                                ActivePlayer* p = player_acquire(character_id);
                                if (p) {
                                    player_send_stats_locked(client_fd, p);
                                    ability_send_data(client_fd, p);
                                    p->is_ready = 1;  // Handshake complete, allow broadcasts
                                    player_release(p);
                                }

                                printf("Account %u, Character %u entered world\n",
                                    account_id, character_id);
                                
                                // Authentication packet consumed, reset buffer
                                buf_len = 0;
                                continue;
                            } else {
                                // Failed to load, remove session
                                session_registry_remove(client_fd);
                            }
                        }
                        
                        // Authentication failed - send error and disconnect
                        WorldConnectAckPacket response = {0};
                        response.header.type = PACKET_WORLD_CONNECT_ACK;
                        response.header.payload_size = htons(sizeof(response) - sizeof(PacketHeader));
                        response.success = 0;
                        strncpy(response.welcome_message, "Invalid ticket", 127);
                        send(client_fd, &response, sizeof(response), 0);
                        break;
                    }
                }
                
                // Not a valid auth packet or wrong packet type - disconnect
                printf("Invalid authentication attempt from fd %d\n", client_fd);
                break;
            } else {
                uint8_t* ptr = buffer;
                ssize_t remaining = buf_len;

                while (remaining >= (ssize_t)sizeof(PacketHeader)) {
                    PacketHeader* header = (PacketHeader*)ptr;

                    // Calculate full packet size (header + payload)
                    size_t packet_size = sizeof(PacketHeader) + ntohs(header->payload_size);

                    if (remaining < (ssize_t)packet_size) {
                        break;  // Incomplete packet, carry over
                    }

                    // Process this packet
                    session_update_activity(client_fd);
                    int pkt_result = process_packet(client_fd, character_id, packet_size, ptr);

                    // Move to next packet
                    ptr += packet_size;
                    remaining -= packet_size;

                    if (pkt_result == -1) goto client_done;  // Clean logout requested
                }

                // Carry over any leftover bytes to the start of the buffer
                if (remaining > 0 && ptr != buffer) {
                    memmove(buffer, ptr, (size_t)remaining);
                }
                buf_len = remaining;
            }
        }
        
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            break;
        }
    }
    
client_done:
    printf("[CLEANUP] fd=%d: cleanup starting (authenticated=%d, char=%u, account=%u)\n",
           client_fd, authenticated, character_id, account_id);

    if (authenticated) {
        // Remove from registry FIRST so a fast reconnect isn't blocked
        // while the (potentially slow) database save runs below.
        session_registry_remove(client_fd);
        g_state.current_players--;

        printf("[CLEANUP] fd=%d: session removed, handling party disconnect\n", client_fd);
        party_handle_disconnect(character_id);

        // Only save/remove if WE still own the active player slot.
        // When a stale session is kicked, the new connection may have already
        // loaded the same character by the time we reach here.  If the slot's
        // client_fd no longer matches ours, the new session owns it — leave it alone.
        printf("[CLEANUP] fd=%d: acquiring player slot for char=%u\n", client_fd, character_id);
        ActivePlayer* player = player_acquire(character_id);
        int do_save = 0;
        CharacterInfo save_data;  // Local copy for lock-free DB save
        if (player) {
            if (player->client_fd == client_fd) {
                printf("[CLEANUP] fd=%d: we own the slot — copying save data for char=%u\n",
                       client_fd, character_id);
                if (player->is_loaded) {
                    // Copy data out quickly while holding the slot lock,
                    // then release the lock so broadcast threads aren't blocked
                    // for the entire duration of the DB write (~1-2s).
                    memset(&save_data, 0, sizeof(save_data));
                    save_data.character_id  = player->character_id;
                    save_data.level         = player->level;
                    save_data.pos_x         = player->pos_x;
                    save_data.pos_y         = player->pos_y;
                    save_data.health        = player->health;
                    save_data.max_health    = player->max_health;
                    save_data.mana          = player->mana;
                    save_data.max_mana      = player->max_mana;
                    save_data.experience    = player->experience;
                    save_data.gold          = player->gold;
                    save_data.helmet        = player->helmet;
                    save_data.gloves        = player->gloves;
                    save_data.chest_armor   = player->chest_armor;
                    save_data.leggings      = player->leggings;
                    save_data.boots         = player->boots;
                    save_data.main_hand     = player->main_hand;
                    save_data.second_hand   = player->second_hand;
                    save_data.blessing      = player->blessing;
                    memcpy(save_data.inventory, player->inventory, sizeof(save_data.inventory));
                    do_save = 1;
                    // Clear dirty flag so player_remove_active won't do a
                    // redundant DB write while holding active_players_lock.
                    player->is_dirty = 0;
                }
                player_release(player);   // Release slot lock NOW, before DB write
                player_remove_active(character_id);
            } else {
                printf("[CLEANUP] fd=%d: slot now owned by fd=%d (new session for char=%u) — skipping save/remove\n",
                       client_fd, player->client_fd, character_id);
                player_release(player);
            }
        } else {
            printf("[CLEANUP] fd=%d: player slot not found for char=%u (already removed?)\n",
                   client_fd, character_id);
        }
        // DB write happens AFTER all locks are released — doesn't block broadcast threads
        if (do_save) {
            printf("[CLEANUP] fd=%d: saving char=%u to DB (lock-free)\n", client_fd, character_id);
            character_update_full_data(&save_data);
        }
        printf("Account %u, Character %u disconnected (fd=%d)\n", account_id, character_id, client_fd);
    }

    printf("[CLEANUP] fd=%d: closing socket\n", client_fd);
    close(client_fd);
    printf("[CLEANUP] fd=%d: thread exiting\n", client_fd);
    return NULL;
}

void* realm_handler_thread(void* arg) {
    int realm_fd = *(int*)arg;
    free(arg);
    
    printf("Realm server connection handler started: fd %d\n", realm_fd);
    
    // First packet should be REALM_AUTH
    RealmAuthPacket auth;
    struct pollfd pfd = {.fd = realm_fd, .events = POLLIN};
    
    // Wait for auth packet with timeout
    if (poll(&pfd, 1, 10000) <= 0) {
        printf("Realm auth timeout\n");
        close(realm_fd);
        return NULL;
    }
    
    if (recv(realm_fd, &auth, sizeof(auth), 0) <= 0) {
        printf("Failed to receive realm auth\n");
        close(realm_fd);
        return NULL;
    }
    
    if (auth.header.type != PACKET_REALM_AUTH) {
        printf("Invalid packet type, expected REALM_AUTH\n");
        close(realm_fd);
        return NULL; 
    }
    
    const char* world_name = g_server.server_name;
    
    // Fetch current key from Redis
    char* current_key = get_server_auth_key_from_redis(world_name);
    if (!current_key) {
        printf("Failed to fetch current auth key from Redis\n");
        
        RealmAuthAckPacket ack = {0};
        ack.header.type = PACKET_REALM_AUTH_ACK;
        ack.header.player_id = 0;
        ack.header.payload_size = 0;
        ack.success = 0;
        strncpy(ack.message, "Server key unavailable", 63);
        
        send(realm_fd, &ack, sizeof(ack), 0);
        close(realm_fd);
        return NULL; 
    }

    // Validate against current key
    if (strcmp(auth.server_key, current_key) != 0) {
        printf("Invalid realm server key from '%s' (key validation failed)\n", auth.realm_name);
        
        RealmAuthAckPacket ack = {0};
        ack.header.type = PACKET_REALM_AUTH_ACK;
        ack.header.player_id = 0;
        ack.header.payload_size = 0;
        ack.success = 0;
        strncpy(ack.message, "Invalid server key", 63);
        
        send(realm_fd, &ack, sizeof(ack), 0);
        free(current_key);
        close(realm_fd);
        return NULL;
    }

    free(current_key);
        
    // Send success response
    RealmAuthAckPacket ack = {0};
    ack.header.type = PACKET_REALM_AUTH_ACK;
    ack.header.player_id = 0;
    ack.header.payload_size = 0;
    ack.success = 1;
    strncpy(ack.message, "Authenticated successfully", 63);
    
    if (send(realm_fd, &ack, sizeof(ack), 0) <= 0) {
        printf("Failed to send auth ack\n");
        close(realm_fd);
        return NULL;
    }
    
    printf("Realm server '%s' authenticated successfully\n", auth.realm_name);
    
    // Handle heartbeats
    pfd.fd = realm_fd;
    pfd.events = POLLIN;
    
    while (g_server.running) {
        // Wait for heartbeat (15 second timeout)
        int ret = poll(&pfd, 1, 15000);
        
        if (ret < 0) {
            if (errno == EINTR) continue;
            printf("Poll error on realm connection\n");
            break;
        }
        
        if (ret == 0) {
            printf("Realm server heartbeat timeout\n");
            break;
        }
        
        if (pfd.revents & POLLIN) {
            WorldHeartbeatPacket hb;
            ssize_t bytes = recv(realm_fd, &hb, sizeof(hb), 0);
            
            if (bytes <= 0) {
                printf("Realm server disconnected\n");
                break;
            }
            
            if (hb.header.type == PACKET_WORLD_HEARTBEAT) {

                WorldStatusPacket status = {0};
                status.header.type = PACKET_WORLD_STATUS;
                status.header.player_id = 0;
                status.header.payload_size = 0;
                
                status.player_count = htonl(g_state.current_players);
                status.max_players = htonl(g_server.max_players);
                status.status = 1; // 1 = online
                status.cpu_usage = 0.0f;
                status.uptime = time(NULL) - g_server_start_time;
                
                // Use server name from config
                strncpy(status.server_name, g_server.server_name, 63);
                
                if (send(realm_fd, &status, sizeof(status), 0) <= 0) {
                    printf("Failed to send status to realm server\n");
                    break;
                }
            } else {
                printf("Unexpected packet type %d from realm server\n", hb.header.type);
            }
        }
        
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            printf("Realm connection error\n");
            break;
        }
    }
    
    close(realm_fd);
    printf("Realm server handler exiting\n");
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
        
        int client_fd = accept(g_server.tcp_sockfd, 
                              (struct sockaddr*)&client_addr, &addr_len);
        
        if (client_fd < 0) continue;

        printf("New connection from %s:%d\n", 
               inet_ntoa(client_addr.sin_addr), 
               ntohs(client_addr.sin_port));

        // Peek at the first packet to determine connection type.
        // Use poll() with a timeout first so the accept thread never blocks
        // indefinitely if a client connects but sends nothing.
        struct pollfd cpfd = {.fd = client_fd, .events = POLLIN};
        if (poll(&cpfd, 1, 3000) <= 0) {
            printf("Peek timeout/error for fd %d — dropping connection\n", client_fd);
            close(client_fd);
            continue;
        }

        uint8_t peek_buffer[16];
        ssize_t peek = recv(client_fd, peek_buffer, sizeof(peek_buffer), MSG_PEEK);

        if (peek > 0) {
            PacketType type = peek_buffer[0];
            
            // Check if this is a realm server connecting
            if (type == PACKET_REALM_AUTH) {
                printf("Detected realm server connection\n");
                
                pthread_t thread;
                int* fd_ptr = malloc(sizeof(int));
                *fd_ptr = client_fd;
                
                if (pthread_create(&thread, NULL, realm_handler_thread, fd_ptr) != 0) {
                    printf("Failed to create realm handler thread\n");
                    close(client_fd);
                    free(fd_ptr);
                } else {
                    pthread_detach(thread);
                }
                continue;
            }
        }
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

void* combat_update_thread(void* arg) {
    (void)arg;
    const long TARGET_INTERVAL_NS = 50000000;  // 50ms = 20Hz
    const double DELTA_TIME = 0.05;             // 50ms as seconds
    struct timespec next_tick, now;

    clock_gettime(CLOCK_MONOTONIC, &next_tick);
    g_combat_running = 1;

    printf("Combat update thread started (20Hz)\n");

    while (g_combat_running && g_server.running) {
        combat_tick(&g_npc_world);           // basic attack resolution + death/respawn
        ability_tick(&g_npc_world, DELTA_TIME); // ability resolution + effects
        projectile_tick(&g_npc_world, DELTA_TIME); // projectile movement + collision
        npc_ai_tick(&g_npc_world, DELTA_TIME);  // NPC AI: targeting, movement, abilities
        loot_tick();                          // despawn expired ground items

        // Calculate next tick time
        next_tick.tv_nsec += TARGET_INTERVAL_NS;
        if (next_tick.tv_nsec >= 1000000000) {
            next_tick.tv_sec++;
            next_tick.tv_nsec -= 1000000000;
        }

        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next_tick, NULL);

        clock_gettime(CLOCK_MONOTONIC, &now);
        long drift_ns = (now.tv_sec - next_tick.tv_sec) * 1000000000 +
                        (now.tv_nsec - next_tick.tv_nsec);
        if (drift_ns > 10000000) {
            printf("[WARNING] Combat thread lagging by %ldms\n", drift_ns / 1000000);
        }
    }

    printf("Combat update thread exiting\n");
    return NULL;
}


void* player_broadcast_thread(void* arg) {
    (void)arg;
    const long TARGET_INTERVAL_NS = 50000000;  // 50ms = 20Hz
    struct timespec next_tick, now;
    
    clock_gettime(CLOCK_MONOTONIC, &next_tick);
    g_player_broadcast_running = 1;
    
    printf("Player broadcast thread started (20Hz)\n");
    
    while (g_player_broadcast_running && g_server.running) {
        // --- Broadcast player positions to nearby players ---
        extern ActivePlayer active_players[];
        extern pthread_mutex_t active_players_lock;

        // Snapshot all active players
        typedef struct {
            int      valid;
            int      client_fd;
            uint32_t character_id;
            float    pos_x, pos_y;
            int32_t  health, max_health;
            uint8_t  player_class;
            uint8_t  is_dead;
        } PlayerSnapshot;

        PlayerSnapshot snapshots[MAX_PLAYERS];

        pthread_mutex_lock(&active_players_lock);
        for (int i = 0; i < MAX_PLAYERS; i++) {
            if (active_players[i].is_loaded) {
                snapshots[i].valid = 1;
                snapshots[i].client_fd = active_players[i].client_fd;
                snapshots[i].character_id = active_players[i].character_id;

                pthread_mutex_lock(&active_players[i].lock);
                snapshots[i].pos_x = active_players[i].pos_x;
                snapshots[i].pos_y = active_players[i].pos_y;
                snapshots[i].health = active_players[i].health;
                snapshots[i].max_health = active_players[i].max_health;
                snapshots[i].player_class = active_players[i].player_class;
                snapshots[i].is_dead = active_players[i].is_dead;
                pthread_mutex_unlock(&active_players[i].lock);
            } else {
                snapshots[i].valid = 0;
            }
        }
        pthread_mutex_unlock(&active_players_lock);

        // For each player, build a packet of nearby players and send
        for (int i = 0; i < MAX_PLAYERS; i++) {
            if (!snapshots[i].valid) continue;

            PlayerPositionBroadcastPacket pkt;
            memset(&pkt, 0, sizeof(pkt));
            pkt.header.type = PACKET_PLAYER_POSITIONS;
            pkt.header.player_id = htonl(snapshots[i].character_id);
            pkt.count = 0;

            float px = snapshots[i].pos_x;
            float py = snapshots[i].pos_y;

            for (int j = 0; j < MAX_PLAYERS; j++) {
                if (!snapshots[j].valid || j == i) continue;
                if (pkt.count >= MAX_NEARBY_PLAYERS) break;

                float dx = snapshots[j].pos_x - px;
                float dy = snapshots[j].pos_y - py;
                if (dx * dx + dy * dy > 800.0f * 800.0f) continue;

                NearbyPlayerData* np = &pkt.players[pkt.count];
                np->player_id = htonl(snapshots[j].character_id);
                np->pos_x = snapshots[j].pos_x;
                np->pos_y = snapshots[j].pos_y;
                np->health = htonl(snapshots[j].health);
                np->max_health = htonl(snapshots[j].max_health);
                np->player_class = snapshots[j].player_class;
                np->is_dead = snapshots[j].is_dead;
                pkt.count++;
            }

            // Only send if there are nearby players
            if (pkt.count > 0) {
                size_t send_size = offsetof(PlayerPositionBroadcastPacket, players) +
                                   pkt.count * sizeof(NearbyPlayerData);
                pkt.header.payload_size = htons((uint16_t)(send_size - sizeof(PacketHeader)));
                send(snapshots[i].client_fd, &pkt, send_size, MSG_NOSIGNAL | MSG_DONTWAIT);
            }
        }

        // Broadcast party updates (HP/mana) for players in parties
        // Track which parties we've already broadcast for this tick
        {
            uint32_t broadcast_parties[MAX_PLAYERS];
            int broadcast_count = 0;

            for (int i = 0; i < MAX_PLAYERS; i++) {
                if (!snapshots[i].valid) continue;

                ActivePlayer* ap = player_acquire(snapshots[i].character_id);
                if (!ap) continue;

                uint32_t pid = ap->party_id;
                player_release(ap);

                if (pid == 0) continue;

                // Check if we already broadcast this party
                int already = 0;
                for (int b = 0; b < broadcast_count; b++) {
                    if (broadcast_parties[b] == pid) { already = 1; break; }
                }
                if (already) continue;

                broadcast_parties[broadcast_count++] = pid;
                party_broadcast_update(pid);
            }
        }

        // Calculate next tick time
        next_tick.tv_nsec += TARGET_INTERVAL_NS;
        if (next_tick.tv_nsec >= 1000000000) {
            next_tick.tv_sec++;
            next_tick.tv_nsec -= 1000000000;
        }
        
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next_tick, NULL);
        
        clock_gettime(CLOCK_MONOTONIC, &now);
        long drift_ns = (now.tv_sec - next_tick.tv_sec) * 1000000000 + 
                        (now.tv_nsec - next_tick.tv_nsec);
        if (drift_ns > 10000000) {
            printf("[WARNING] Player broadcast thread lagging by %ldms\n", 
                   drift_ns / 1000000);
        }
    }
    
    printf("Player broadcast thread exiting\n");
    return NULL;
}

void broadcast_npc_positions_to_player(int client_fd, uint32_t character_id, NPCWorld* world) {
    // Find player
    ActivePlayer* player = player_acquire(character_id);
    if (!player) return;
    if (!player->is_loaded) { player_release(player); return; }

    float px = player->pos_x;
    float py = player->pos_y;
    player_release(player);
    
    // Build packet with NPCs within 500 units
    NPCPositionPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.header.type = PACKET_NPC_POSITIONS;
    pkt.header.player_id = htonl(character_id);
    pkt.header.payload_size = 0;
    pkt.npc_count = 0;
    
    pthread_mutex_lock(&world->lock);
    
    for (int i = 0; i < MAX_NPCS && pkt.npc_count < MAX_NPCS_PER_PACKET; i++) {
        NPCEntity* npc = &world->npcs[i];
        if (npc->id == 0) continue;  // Empty slot
        
        // Distance check (500 unit radius)
        float dx = npc->pos_x - px;
        float dy = npc->pos_y - py;
        float dist_sq = dx*dx + dy*dy;
        
        if (dist_sq <= 2000.0f * 2000.0f) {
            NPCPositionData* data = &pkt.npcs[pkt.npc_count];
            data->npc_id = htonl(npc->id);
            data->pos_x = npc->pos_x;
            data->pos_y = npc->pos_y;
            data->health = htonl(npc->health);
            data->max_health = htonl(npc->max_health);
            data->is_alive = npc->is_alive;
            data->category = npc->category;
            data->is_interactable = npc->is_interactable;
            data->npc_type_id = (uint8_t)npc->npc_type_id;
            pkt.npc_count++;
        }
    }

    pthread_mutex_unlock(&world->lock);

    // Only send if there are NPCs — send only actual data
    if (pkt.npc_count > 0) {
        size_t send_size = offsetof(NPCPositionPacket, npcs) +
                           pkt.npc_count * sizeof(NPCPositionData);
        pkt.header.payload_size = htons((uint16_t)(send_size - sizeof(PacketHeader)));
        send(client_fd, &pkt, send_size, MSG_NOSIGNAL | MSG_DONTWAIT);
    }
}

// Modify the npc_broadcast_thread function:
void* npc_broadcast_thread(void* arg) {
    (void)arg;
    const long TARGET_INTERVAL_NS = 100000000;  // 100ms = 10Hz
    struct timespec next_tick, now;
    
    clock_gettime(CLOCK_MONOTONIC, &next_tick);
    g_npc_broadcast_running = 1;
    
    printf("NPC broadcast thread started (10Hz)\n");
    
    while (g_npc_broadcast_running && g_server.running) {
        // Collect player info while holding the lock, then release before sending
        extern ActivePlayer active_players[];
        extern pthread_mutex_t active_players_lock;
        
        // Temporary storage for player data
        struct {
            int client_fd;
            uint32_t character_id;
            float pos_x;
            float pos_y;
            int valid;
        } players_snapshot[MAX_PLAYERS];
        
        // Take a snapshot of active players
        pthread_mutex_lock(&active_players_lock);
        for (int i = 0; i < MAX_PLAYERS; i++) {
            if (active_players[i].is_loaded && active_players[i].is_ready) {
                players_snapshot[i].valid = 1;
                players_snapshot[i].client_fd = active_players[i].client_fd;
                players_snapshot[i].character_id = active_players[i].character_id;
                
                pthread_mutex_lock(&active_players[i].lock);
                players_snapshot[i].pos_x = active_players[i].pos_x;
                players_snapshot[i].pos_y = active_players[i].pos_y;
                pthread_mutex_unlock(&active_players[i].lock);
            } else {
                players_snapshot[i].valid = 0;
            }
        }
        pthread_mutex_unlock(&active_players_lock);
        
        // Now send to each player WITHOUT holding the players lock
        for (int i = 0; i < MAX_PLAYERS; i++) {
            if (!players_snapshot[i].valid) continue;
            
            // Build NPC packet for this player
            NPCPositionPacket pkt;
            memset(&pkt, 0, sizeof(pkt));
            pkt.header.type = PACKET_NPC_POSITIONS;
            pkt.header.player_id = htonl(players_snapshot[i].character_id);
            pkt.header.payload_size = 0;
            pkt.npc_count = 0;
            
            float px = players_snapshot[i].pos_x;
            float py = players_snapshot[i].pos_y;
            
            pthread_mutex_lock(&g_npc_world.lock);
            for (int n = 0; n < MAX_NPCS && pkt.npc_count < MAX_NPCS_PER_PACKET; n++) {
                NPCEntity* npc = &g_npc_world.npcs[n];
                if (npc->id == 0) continue;
                
                float dx = npc->pos_x - px;
                float dy = npc->pos_y - py;
                float dist_sq = dx*dx + dy*dy;
                
                if (dist_sq <= 2000.0f * 2000.0f) {
                    NPCPositionData* data = &pkt.npcs[pkt.npc_count];
                    data->npc_id = htonl(npc->id);
                    data->pos_x = npc->pos_x;
                    data->pos_y = npc->pos_y;
                    data->health = htonl(npc->health);
                    data->max_health = htonl(npc->max_health);
                    data->is_alive = npc->is_alive;
                    data->category = npc->category;
                    data->is_interactable = npc->is_interactable;
                    data->npc_type_id = (uint8_t)npc->npc_type_id;
                    pkt.npc_count++;
                }
            }
            pthread_mutex_unlock(&g_npc_world.lock);
            
            if (pkt.npc_count > 0) {
                size_t send_size = offsetof(NPCPositionPacket, npcs) +
                                   pkt.npc_count * sizeof(NPCPositionData);
                pkt.header.payload_size = htons((uint16_t)(send_size - sizeof(PacketHeader)));
                static int s_npc_send_logged = 0;
                if (!s_npc_send_logged) {
                    printf("[NPC_BROADCAST] First NPC packet sent: %d NPCs to player %u\n",
                           pkt.npc_count, players_snapshot[i].character_id);
                    s_npc_send_logged = 1;
                }
                send(players_snapshot[i].client_fd, &pkt, send_size, MSG_NOSIGNAL | MSG_DONTWAIT);
            }
        }
        
        // Calculate next tick time
        next_tick.tv_nsec += TARGET_INTERVAL_NS;
        if (next_tick.tv_nsec >= 1000000000) {
            next_tick.tv_sec++;
            next_tick.tv_nsec -= 1000000000;
        }
        
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next_tick, NULL);
        
        clock_gettime(CLOCK_MONOTONIC, &now);
        long drift_ns = (now.tv_sec - next_tick.tv_sec) * 1000000000 + 
                        (now.tv_nsec - next_tick.tv_nsec);
        if (drift_ns > 10000000) {
            printf("[WARNING] NPC broadcast thread lagging by %ldms\n", 
                   drift_ns / 1000000);
        }
    }
    
    printf("NPC broadcast thread exiting\n");
    return NULL;
}

void* projectile_broadcast_thread(void* arg) {
    (void)arg;
    const long TARGET_INTERVAL_NS = 33333333;  // ~33ms = 30Hz
    struct timespec next_tick, now;
    
    clock_gettime(CLOCK_MONOTONIC, &next_tick);
    g_projectile_broadcast_running = 1;
    
    printf("Projectile broadcast thread started (30Hz)\n");
    
    while (g_projectile_broadcast_running && g_server.running) {
        projectile_broadcast();
        
        // Calculate next tick time
        next_tick.tv_nsec += TARGET_INTERVAL_NS;
        if (next_tick.tv_nsec >= 1000000000) {
            next_tick.tv_sec++;
            next_tick.tv_nsec -= 1000000000;
        }
        
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next_tick, NULL);
        
        clock_gettime(CLOCK_MONOTONIC, &now);
        long drift_ns = (now.tv_sec - next_tick.tv_sec) * 1000000000 + 
                        (now.tv_nsec - next_tick.tv_nsec);
        if (drift_ns > 10000000) {
            printf("[WARNING] Projectile broadcast thread lagging by %ldms\n", 
                   drift_ns / 1000000);
        }
    }
    
    printf("Projectile broadcast thread exiting\n");
    return NULL;
}

void signal_handler(int signum) {
    printf("\n[SIGNAL] Received signal %d, shutting down gracefully...\n", signum);
    g_server.running = 0;
    g_combat_running = 0;
    g_player_broadcast_running = 0;
    g_npc_broadcast_running = 0;
    g_projectile_broadcast_running = 0;
}

int main(int argc, char** argv) {
    (void)argc;

    printf("=== WORLD SERVER ===\n");
    printf("PID: %d\n", getpid());

    init_data_paths();

    g_server_start_time = time(NULL);
    session_init();
    session_registry_init();
    
    memset(&g_server, 0, sizeof(g_server));
    memset(&g_state, 0, sizeof(g_state));
    
    // Initialize g_server.running BEFORE setting up signals
    g_server.running = 1;
    
    // Ignore SIGPIPE — prevents crash when broadcast threads write to a client
    // socket that has been closed or reset (common on player disconnect).
    signal(SIGPIPE, SIG_IGN);

    // Install signal handlers
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    sigaction(SIGINT, &sa, NULL);   // Ctrl+C
    sigaction(SIGTERM, &sa, NULL);  // kill command

    printf("[SIGNAL] Signal handlers installed\n");
    
    // Connect to Redis
    const char* redis_host = getenv("REDIS_HOST");
    if (!redis_host) redis_host = "127.0.0.1";
    
    int redis_port = 6379;
    const char* redis_port_str = getenv("REDIS_PORT");
    if (redis_port_str) redis_port = atoi(redis_port_str);
    
    printf("Connecting to Redis at %s:%d...\n", redis_host, redis_port);
    g_redis = redisConnect(redis_host, redis_port);
    
    if (!g_redis || g_redis->err) {
        if (g_redis) {
            fprintf(stderr, "Redis connection error: %s\n", g_redis->errstr);
            redisFree(g_redis);
        } else {
            fprintf(stderr, "Redis connection error: can't allocate context\n"); 
        }
        return 1;
    }
    
    printf("✓ Connected to Redis\n");
    
    // Parse config file
    if(argv && argv[1]) {
        if(!set_config(argv[1])) {
            printf("Usage: ./world_server <server>.conf\n");
            exit(1);
        }
    } else {
        printf("Usage: ./world_server <server>.conf\n");
        exit(1);
    }

    if (!items_init(DATA_PATH)) {
        fprintf(stderr, "FAILED - Item system initialization\n");
        return 1;
    }

    if (!abilities_init(ABILITIES_PATH)) {
        fprintf(stderr, "FAILED - Ability system initialization\n");
        return 1;
    }

    ability_handler_init();
    projectile_init();

    if (!loot_init(DATA_PATH)) {
        fprintf(stderr, "FAILED - Loot system initialization\n");
        return 1;
    }

    if (!npc_ai_init(NPC_TYPES_PATH)) {
        fprintf(stderr, "FAILED - NPC AI system initialization\n");
        return 1;
    }

    printf("Loading dialogue system... ");
    fflush(stdout);
    if (!dialogue_system_init(DIALOGUES_PATH)) {
        fprintf(stderr, "FAILED - Dialogue system initialization\n");
        return 1;
    }
    printf("OK (%d dialogues loaded)\n", dialogues_get_count());

    quest_system_set_dir(QUEST_SAVE_DIR);
    printf("Loading quest system... ");
    fflush(stdout);
    quest_system_init(QUESTS_PATH);
    printf("OK\n");

    printf("Loading shop system... ");
    fflush(stdout);
    shop_init(SHOPS_PATH);
    printf("OK\n");

    // Connect to database
    printf("Connecting to database for world '%s'\n", g_server.server_name);
    
    const char* pg_conn_str = get_database_for_world(g_server.server_name);
    if (!pg_conn_str) {
        fprintf(stderr, "FAILED - No database configured for world '%s'\n", 
                g_server.server_name);
        return 1;
    }
    
    printf("Database connection string: %s\n", pg_conn_str);

    class_stats_init();
    combat_profiles_load(ATTACK_PROFILES_PATH);
    
    if (!playerdata_init(pg_conn_str)) {
        printf("FAILED - PostgreSQL initialization\n");
        return 1;
    }
    
    printf("✓ Connected to world database\n");

    combat_npc_init(&g_npc_world);
    party_init();

    // Load NPC spawns from data file
    {
        int spawn_count = npc_spawns_load(SPAWNS_PATH, &g_npc_world);
        if (spawn_count < 0) {
            fprintf(stderr, "FAILED - Could not load NPC spawns from %s\n", SPAWNS_PATH);
            return 1;
        }
        printf("Loaded %d NPC spawns\n", spawn_count);
    }
    
    if (!playerdata_start_save_thread()) {
        fprintf(stderr, "FAILED - Periodic save thread\n");
        playerdata_close();
        return 1;
    }

    g_server.tcp_sockfd = create_tcp_server_socket(g_server.port);
    if (g_server.tcp_sockfd < 0) {
        playerdata_close();
        return 1;
    }
    
    // START ACCEPT THREAD
    if (pthread_create(&g_server.accept_thread, NULL, accept_thread_func, NULL) != 0) {
        fprintf(stderr, "FAILED - Accept thread\n");
        close(g_server.tcp_sockfd);
        playerdata_close();
        return 1;
    }
    
    // START COMBAT THREAD (20Hz)
    if (pthread_create(&g_combat_thread, NULL, combat_update_thread, NULL) != 0) {
        fprintf(stderr, "FAILED - Combat update thread\n");
        g_server.running = 0;
        pthread_join(g_server.accept_thread, NULL);
        close(g_server.tcp_sockfd);
        playerdata_close();
        return 1;
    }
    
    // START PLAYER BROADCAST THREAD (20Hz)
    if (pthread_create(&g_player_broadcast_thread, NULL, player_broadcast_thread, NULL) != 0) {
        fprintf(stderr, "FAILED - Player broadcast thread\n");
        g_combat_running = 0;
        g_server.running = 0;
        pthread_join(g_combat_thread, NULL);
        pthread_join(g_server.accept_thread, NULL);
        close(g_server.tcp_sockfd);
        playerdata_close();
        return 1;
    }
    
    // START NPC BROADCAST THREAD (10Hz)
    if (pthread_create(&g_npc_broadcast_thread, NULL, npc_broadcast_thread, NULL) != 0) {
        fprintf(stderr, "FAILED - NPC broadcast thread\n");
        g_player_broadcast_running = 0;
        g_combat_running = 0;
        g_server.running = 0;
        pthread_join(g_player_broadcast_thread, NULL);
        pthread_join(g_combat_thread, NULL);
        pthread_join(g_server.accept_thread, NULL);
        close(g_server.tcp_sockfd);
        playerdata_close();
        return 1;
    }
    
    // START PROJECTILE BROADCAST THREAD (30Hz)
    if (pthread_create(&g_projectile_broadcast_thread, NULL, projectile_broadcast_thread, NULL) != 0) {
        fprintf(stderr, "FAILED - Projectile broadcast thread\n");
        g_npc_broadcast_running = 0;
        g_player_broadcast_running = 0;
        g_combat_running = 0;
        g_server.running = 0;
        pthread_join(g_npc_broadcast_thread, NULL);
        pthread_join(g_player_broadcast_thread, NULL);
        pthread_join(g_combat_thread, NULL);
        pthread_join(g_server.accept_thread, NULL);
        close(g_server.tcp_sockfd);
        playerdata_close();
        return 1;
    }
    
    printf("✓ All systems online - server ready\n");
    printf("  - Accept thread: Running\n");
    printf("  - Combat thread: 20Hz\n");
    printf("  - Player broadcast: 20Hz\n");
    printf("  - NPC broadcast: 10Hz\n");
    printf("  - Projectile broadcast: 30Hz\n");
    
    // MAIN THREAD: Wait for shutdown signal
    while (g_server.running) {
        sleep(1);
        
        // Print server stats every 10 seconds
        static time_t last_stats = 0;
        time_t now = time(NULL);
        if (now - last_stats >= 10) {
            printf("[STATS] Players: %d, Uptime: %lds\n", 
                   g_state.current_players, now - g_server_start_time);
            last_stats = now;
        }
    }
    
    printf("\nShutting down...\n");
    
    // Stop all broadcast threads
    g_projectile_broadcast_running = 0;
    g_npc_broadcast_running = 0;
    g_player_broadcast_running = 0;
    g_combat_running = 0;
    
    pthread_join(g_projectile_broadcast_thread, NULL);
    pthread_join(g_npc_broadcast_thread, NULL);
    pthread_join(g_player_broadcast_thread, NULL);
    pthread_join(g_combat_thread, NULL);
    
    // Stop accept thread
    close(g_server.tcp_sockfd);
    pthread_join(g_server.accept_thread, NULL);
    
    // Cleanup
    npc_ai_cleanup();
    loot_cleanup();
    projectile_cleanup();
    ability_handler_cleanup();
    abilities_cleanup();
    dialogue_system_cleanup();
    items_cleanup();
    playerdata_stop_save_thread();
    playerdata_close();
    
    if (g_redis) {
        redisFree(g_redis);
        g_redis = NULL;
    }

    printf("World Server stopped\n");
    return 0;
}