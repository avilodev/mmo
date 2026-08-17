/**
 * @file
 * Initialize the world service and coordinate gameplay, networking, and broadcast threads.
 */
#include "types.h"
#include "log.h"
#include "packet_limiter.h"
#include "limit_profiles.h"
#include "net_notify.h"
#include "net_loop.h"
#include "session.h"
#include "realm_world_auth.h"
#include "world_database_config.h"

#include "ability_def.h"
#include "ability_handler.h"
#include "class_stats.h"
#include "connection_io.h"
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
#include "spatial_grid.h"
#include "broadcast_snapshot.h"
#include "tick_scheduler.h"
#include "party.h"
#include "quest_system.h"
#include "shop.h"
#include "utils.h"
#include "world_collision.h"
#include "zone_system.h"

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
static char WORLD_DAT_PATH[512];
static char ZONES_PATH[512];

/** Build one bounded runtime asset path or terminate startup on overflow. */
static void set_data_path(char* destination, size_t destination_size,
                          const char* directory, const char* relative_path) {
    size_t directory_length = strlen(directory);
    size_t relative_length = strlen(relative_path);

    if (directory_length + relative_length + 1 > destination_size) {
        fprintf(stderr, "Runtime data path is too long: %s%s\n",
                directory, relative_path);
        exit(EXIT_FAILURE);
    }

    memcpy(destination, directory, directory_length);
    memcpy(destination + directory_length, relative_path, relative_length + 1);
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

/** Resolve all world runtime assets relative to the server executable. */
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
    set_data_path(DATA_PATH,            sizeof(DATA_PATH),            exe, "/data/items.json");
    set_data_path(ABILITIES_PATH,       sizeof(ABILITIES_PATH),       exe, "/data/abilities.json");
    set_data_path(DIALOGUES_PATH,       sizeof(DIALOGUES_PATH),       exe, "/data/dialogues");
    set_data_path(NPC_TYPES_PATH,       sizeof(NPC_TYPES_PATH),       exe, "/data/npc_types.json");
    set_data_path(SPAWNS_PATH,          sizeof(SPAWNS_PATH),          exe, "/data/spawns.json");
    set_data_path(ATTACK_PROFILES_PATH, sizeof(ATTACK_PROFILES_PATH), exe, "/data/attack_profiles.json");
    set_data_path(QUESTS_PATH,          sizeof(QUESTS_PATH),          exe, "/data/quests.json");
    set_data_path(SHOPS_PATH,           sizeof(SHOPS_PATH),           exe, "/data/shops.json");
    set_data_path(ZONES_PATH,           sizeof(ZONES_PATH),           exe, "/data/zones.json");
    set_data_path(QUEST_SAVE_DIR,       sizeof(QUEST_SAVE_DIR),       exe, "/data/quests");
    // Runtime assets are packaged beside the binary by the Makefile.
    set_data_path(WORLD_DAT_PATH,       sizeof(WORLD_DAT_PATH),       exe, "/data/world.dat");
    printf("[PATHS] Data directory: %s/data/\n", exe);
}

static pthread_t g_combat_thread;

// One thread now carries every outbound stream — see the WORLD BROADCAST block
// below for why they were merged.
static pthread_t g_broadcast_thread;

static volatile int g_combat_running = 0;
static volatile int g_broadcast_running = 0;

// Global for tracking world server uptime
time_t g_server_start_time = 0; 

NPCWorld g_npc_world;

/**
 * Authenticate one realm connection and answer its world-status heartbeats.
 *
 * The function owns and frees the heap-allocated descriptor argument, then closes the realm descriptor before returning.
 *
 * @return      Always NULL.
 */
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
    
    if (recv_exact_timeout(realm_fd, &auth, sizeof(auth), 10000) != (ssize_t)sizeof(auth)) {
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
        
        server_send_direct(realm_fd, &ack, sizeof(ack));
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
        
        server_send_direct(realm_fd, &ack, sizeof(ack));
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
    
    if (server_send_direct(realm_fd, &ack, sizeof(ack)) <= 0) {
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
            ssize_t bytes = recv_exact_timeout(realm_fd, &hb, sizeof(hb), 15000);
            
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
                
                if (server_send_direct(realm_fd, &status, sizeof(status)) <= 0) {
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

/**
 * Classify accepted realm connections and submit player connections to event loops.
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
        // Hand the socket to an event loop. It is pinned to loop (fd % N) for
        // its whole life, so exactly one thread ever services it.
        net_loop_submit(client_fd);
    }
    
    return NULL;
}

/** Number of measured phases in each gameplay tick. */
#define TICK_PHASE_COUNT 6
static const char* g_phase_name[TICK_PHASE_COUNT] = {
    "snapshot", "combat", "ability", "projectile", "npc_ai", "loot"
};
static double g_phase_total_ms[TICK_PHASE_COUNT];
static double g_phase_worst_ms[TICK_PHASE_COUNT];
static long   g_phase_samples;

static inline double mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

/** Log rolling mean and worst phase times, then clear the timing window. */
static void tick_phase_report(void) {
    if (g_phase_samples == 0) return;

    char line[512];
    int  n = snprintf(line, sizeof(line), "[TICK] over %ld ticks —", g_phase_samples);
    double mean_total = 0.0;

    for (int p = 0; p < TICK_PHASE_COUNT && n < (int)sizeof(line); p++) {
        double mean = g_phase_total_ms[p] / (double)g_phase_samples;
        mean_total += mean;
        n += snprintf(line + n, sizeof(line) - (size_t)n, " %s %.2f/%.2f",
                      g_phase_name[p], mean, g_phase_worst_ms[p]);
    }
    if (n < (int)sizeof(line))
        snprintf(line + n, sizeof(line) - (size_t)n,
                 " | total mean %.2fms of 50ms budget", mean_total);

    LOG_INFO("%s", line);
    LOG_DEBUG("[TICK] figures are mean/worst milliseconds per phase");

    memset(g_phase_total_ms, 0, sizeof(g_phase_total_ms));
    memset(g_phase_worst_ms, 0, sizeof(g_phase_worst_ms));
    g_phase_samples = 0;
}

/**
 * Run snapshot, combat, ability, projectile, NPC, and loot updates at 20 Hz.
 *
 * @return      Always NULL after shutdown or snapshot initialization failure.
 */
void* combat_update_thread(void* arg) {
    (void)arg;
    const long TARGET_INTERVAL_NS = 50000000;  // 50ms = 20Hz
    const double DELTA_TIME = 0.05;             // 50ms as seconds
    struct timespec next_tick, now;

    clock_gettime(CLOCK_MONOTONIC, &next_tick);
    g_combat_running = 1;

    // One snapshot of the players, shared by every phase of the tick. See
    // tick_snapshot.h for why the phases must not sample independently.
    static TickSnapshot snapshot;
    if (!tick_snapshot_init(&snapshot)) {
        LOG_ERROR("[TICK] gameplay thread cannot start without an interest grid");
        return NULL;
    }

    double phase_ms[TICK_PHASE_COUNT];
    double last_report = mono_ms();

    printf("Combat update thread started (20Hz)\n");

    while (g_combat_running && g_server.running) {
        double t0 = mono_ms(), t1;

        tick_snapshot_build(&snapshot);       t1 = mono_ms(); phase_ms[0] = t1 - t0; t0 = t1;
        combat_tick(&g_npc_world);            t1 = mono_ms(); phase_ms[1] = t1 - t0; t0 = t1;
        ability_tick(&g_npc_world, DELTA_TIME);            t1 = mono_ms(); phase_ms[2] = t1 - t0; t0 = t1;
        projectile_tick(&g_npc_world, &snapshot, DELTA_TIME); t1 = mono_ms(); phase_ms[3] = t1 - t0; t0 = t1;
        npc_ai_tick(&g_npc_world, &snapshot, DELTA_TIME);  t1 = mono_ms(); phase_ms[4] = t1 - t0; t0 = t1;
        loot_tick();                          t1 = mono_ms(); phase_ms[5] = t1 - t0;

        for (int p = 0; p < TICK_PHASE_COUNT; p++) {
            g_phase_total_ms[p] += phase_ms[p];
            if (phase_ms[p] > g_phase_worst_ms[p]) g_phase_worst_ms[p] = phase_ms[p];
        }
        g_phase_samples++;

        if (t1 - last_report >= 10000.0) {
            tick_phase_report();
            last_report = t1;
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
            printf("[WARNING] Combat thread lagging by %ldms\n", drift_ns / 1000000);
        }
    }

    tick_phase_report();          // final numbers before the counters go away
    tick_snapshot_free(&snapshot);

    printf("Combat update thread exiting\n");
    return NULL;
}


/** Player-to-player interest radius in world pixels. */
#define PLAYER_VIEW_RADIUS 800.0f

/** Player-to-NPC interest radius in world pixels. */
#define NPC_VIEW_RADIUS 2000.0f

/** Own one shared player snapshot and broadcast-task scratch storage. */
typedef struct {
    BroadcastPlayer   players[MAX_PLAYERS];
    SpatialPoint      player_points[MAX_PLAYERS];
    BroadcastSnapshot snapshot;

    // NPC scratch. Only the NPC task reads this, so it is filled there rather
    // than in prepare — sampling 256 NPCs on every 30Hz pass to feed a 10Hz
    // stream would be two thirds wasted.
    NPCPositionData   npc_wire[MAX_NPCS];
    SpatialPoint      npc_points[MAX_NPCS];
    SpatialGrid*      npc_grid;
} BroadcastContext;

/** Snapshot active players and rebuild their interest grid for one scheduler pass. */
static void broadcast_prepare(void* ctx) {
    BroadcastContext* bc = ctx;

    extern ActivePlayer active_players[];

    int count = 0;

    // Walk the online players, not all MAX_PLAYERS slots. This runs 30 times a
    // second: at 30 players it is 30 iterations instead of 1000, and the cost
    // now tracks who is actually here rather than the size of the table.
    player_registry_rdlock();
    int online_count = 0;
    const int* online = player_active_list_locked(&online_count);
    for (int n = 0; n < online_count; n++) {
        int i = online[n];
        if (!active_players[i].is_loaded) continue;   // reserved by an in-flight login

        BroadcastPlayer* snap = &bc->players[count];
        snap->client_fd = active_players[i].client_fd;
        snap->character_id = active_players[i].character_id;

        pthread_mutex_lock(&active_players[i].lock);
        snap->pos_x = active_players[i].pos_x;
        snap->pos_y = active_players[i].pos_y;
        snap->health = active_players[i].health;
        snap->max_health = active_players[i].max_health;
        snap->player_class = active_players[i].player_class;
        snap->player_race = (uint8_t)active_players[i].player_race;
        snap->level = (uint8_t)active_players[i].level;
        snap->is_dead = active_players[i].is_dead;
        snap->ping_ms = active_players[i].ping_ms;
        // Carried in the snapshot so the party stream does not have to look
        // each player up again — that was one index lookup per player per tick
        // for a field already sitting right here.
        snap->party_id = active_players[i].party_id;
        pthread_mutex_unlock(&active_players[i].lock);

        bc->player_points[count].x = snap->pos_x;
        bc->player_points[count].y = snap->pos_y;
        count++;
    }
    player_registry_unlock();

    bc->snapshot.count = count;
    spatial_grid_build(bc->snapshot.grid, bc->player_points, count);
}

/** Broadcast nearby player state and one update per represented party. */
static void task_broadcast_players(void* ctx) {
    BroadcastContext* bc = ctx;
    const BroadcastSnapshot* snap = &bc->snapshot;

    for (int i = 0; i < snap->count; i++) {
        PlayerPositionBroadcastPacket pkt;
        memset(&pkt, 0, sizeof(pkt));
        pkt.header.type = PACKET_PLAYER_POSITIONS;
        pkt.header.player_id = htonl(snap->players[i].character_id);
        pkt.count = 0;

        // One extra slot: the viewer is in the grid too and comes back as its
        // own nearest result, so without it a full crowd would cost this
        // player one visible neighbour.
        int nearby[MAX_NEARBY_PLAYERS + 1];
        int nearby_count = spatial_grid_query(snap->grid,
                                              snap->players[i].pos_x,
                                              snap->players[i].pos_y,
                                              PLAYER_VIEW_RADIUS,
                                              nearby, MAX_NEARBY_PLAYERS + 1);

        // nearby[] is nearest-first, so stopping at the packet limit drops the
        // most distant players rather than an arbitrary set — the cap behaves
        // like a view radius instead of like a slot-order accident.
        for (int k = 0; k < nearby_count && pkt.count < MAX_NEARBY_PLAYERS; k++) {
            int j = nearby[k];
            if (j == i) continue;

            NearbyPlayerData* np = &pkt.players[pkt.count];
            np->player_id = htonl(snap->players[j].character_id);
            np->pos_x = snap->players[j].pos_x;
            np->pos_y = snap->players[j].pos_y;
            np->health = htonl(snap->players[j].health);
            np->max_health = htonl(snap->players[j].max_health);
            np->player_class = snap->players[j].player_class;
            np->player_race = snap->players[j].player_race;
            np->level = snap->players[j].level;
            np->is_dead = snap->players[j].is_dead;
            np->ping_ms = htons(snap->players[j].ping_ms);
            pkt.count++;
        }

        if (pkt.count > 0) {
            size_t send_size = offsetof(PlayerPositionBroadcastPacket, players) +
                               pkt.count * sizeof(NearbyPlayerData);
            pkt.header.payload_size = htons((uint16_t)(send_size - sizeof(PacketHeader)));
            server_send(snap->players[i].client_fd, &pkt, send_size);
        }
    }

    // Party HP/mana updates, one broadcast per party per tick.
    uint32_t broadcast_parties[MAX_PLAYERS];
    int broadcast_count = 0;

    for (int i = 0; i < snap->count; i++) {
        uint32_t pid = snap->players[i].party_id;
        if (pid == 0) continue;

        int already = 0;
        for (int b = 0; b < broadcast_count; b++) {
            if (broadcast_parties[b] == pid) { already = 1; break; }
        }
        if (already) continue;

        broadcast_parties[broadcast_count++] = pid;
        party_broadcast_update(pid);
    }
}

/** Snapshot NPCs and broadcast nearest in-range entries to each player. */
static void task_broadcast_npcs(void* ctx) {
    BroadcastContext* bc = ctx;
    const BroadcastSnapshot* snap = &bc->snapshot;

    // Sample the NPCs once, under one lock acquisition. The previous version
    // took this lock once per player per tick.
    int npc_count = 0;

    pthread_mutex_lock(&g_npc_world.lock);
    for (int n = 0; n < MAX_NPCS; n++) {
        NPCEntity* npc = &g_npc_world.npcs[n];
        if (npc->id == 0) continue;

        // Byte-swapped once here rather than once per recipient.
        NPCPositionData* data = &bc->npc_wire[npc_count];
        data->npc_id = htonl(npc->id);
        data->pos_x = npc->pos_x;
        data->pos_y = npc->pos_y;
        data->health = htonl(npc->health);
        data->max_health = htonl(npc->max_health);
        data->is_alive = npc->is_alive;
        data->category = npc->category;
        data->is_interactable = npc->is_interactable;
        data->npc_type_id = (uint8_t)npc->npc_type_id;

        bc->npc_points[npc_count].x = npc->pos_x;
        bc->npc_points[npc_count].y = npc->pos_y;
        npc_count++;
    }
    pthread_mutex_unlock(&g_npc_world.lock);

    if (npc_count == 0) return;

    spatial_grid_build(bc->npc_grid, bc->npc_points, npc_count);

    for (int i = 0; i < snap->count; i++) {
        NPCPositionPacket pkt;
        memset(&pkt, 0, sizeof(pkt));
        pkt.header.type = PACKET_NPC_POSITIONS;
        pkt.header.player_id = htonl(snap->players[i].character_id);
        pkt.header.payload_size = 0;
        pkt.npc_count = 0;

        int nearby[MAX_NPCS_PER_PACKET];
        int nearby_count = spatial_grid_query(bc->npc_grid,
                                              snap->players[i].pos_x,
                                              snap->players[i].pos_y,
                                              NPC_VIEW_RADIUS,
                                              nearby, MAX_NPCS_PER_PACKET);

        // Nearest-first, so a player in a dense spawn gets the NPCs actually
        // around them rather than the lowest slot indices.
        for (int k = 0; k < nearby_count; k++) {
            pkt.npcs[pkt.npc_count++] = bc->npc_wire[nearby[k]];
        }

        if (pkt.npc_count > 0) {
            size_t send_size = offsetof(NPCPositionPacket, npcs) +
                               pkt.npc_count * sizeof(NPCPositionData);
            pkt.header.payload_size = htons((uint16_t)(send_size - sizeof(PacketHeader)));
            server_send(snap->players[i].client_fd, &pkt, send_size);
        }
    }
}

static void task_broadcast_projectiles(void* ctx) {
    BroadcastContext* bc = ctx;
    projectile_broadcast(&bc->snapshot);
}

/**
 * Schedule player, NPC, and projectile broadcasts from one consistent snapshot stream.
 *
 * Broadcast tasks must not block because they share this scheduler thread.
 *
 * @return      Always NULL after shutdown or allocation failure.
 */
void* world_broadcast_thread(void* arg) {
    (void)arg;

    // Heap, not stack: the context is roughly 50KB of snapshot arrays.
    BroadcastContext* bc = calloc(1, sizeof(*bc));
    if (!bc) {
        LOG_ERROR("[BROADCAST] could not allocate the pass context");
        return NULL;
    }

    float world_w = 0.0f, world_h = 0.0f;
    world_collision_extent(&world_w, &world_h);

    bc->snapshot.players = bc->players;
    bc->snapshot.grid = spatial_grid_create(world_w, world_h,
                                            SPATIAL_GRID_DEFAULT_CELL, MAX_PLAYERS);
    bc->npc_grid = spatial_grid_create(world_w, world_h,
                                       SPATIAL_GRID_DEFAULT_CELL, MAX_NPCS);
    if (!bc->snapshot.grid || !bc->npc_grid) {
        LOG_ERROR("[BROADCAST] could not allocate the interest grids");
        spatial_grid_destroy(bc->snapshot.grid);
        spatial_grid_destroy(bc->npc_grid);
        free(bc);
        return NULL;
    }

    // registration order determines overload shedding order
    TickScheduler scheduler;
    tick_scheduler_init(&scheduler, broadcast_prepare, bc);
    tick_scheduler_add(&scheduler, "projectiles", 33333333L, task_broadcast_projectiles);
    tick_scheduler_add(&scheduler, "players",     50000000L, task_broadcast_players);
    tick_scheduler_add(&scheduler, "npcs",       100000000L, task_broadcast_npcs);

    {
        int cols = 0, rows = 0;
        spatial_grid_dimensions(bc->snapshot.grid, &cols, &rows);
        LOG_INFO("[BROADCAST] thread started — projectiles 30Hz, players 20Hz, "
                 "npcs 10Hz, %dx%d interest grid @ %.0fpx",
                 cols, rows, SPATIAL_GRID_DEFAULT_CELL);
    }

    // The flag is raised by main() before this thread is created, not here:
    // setting it here would let a shutdown signal that arrives during startup
    // be overwritten, leaving the loop running with nothing to stop it.
    tick_scheduler_run(&scheduler, &g_broadcast_running);

    tick_scheduler_report(&scheduler);

    spatial_grid_destroy(bc->snapshot.grid);
    spatial_grid_destroy(bc->npc_grid);
    free(bc);

    LOG_INFO("[BROADCAST] thread exiting");
    return NULL;
}

/** Request shutdown of the world server and its gameplay threads. */
void signal_handler(int signum) {
    printf("\n[SIGNAL] Received signal %d, shutting down gracefully...\n", signum);
    g_server.running = 0;
    g_combat_running = 0;
    g_broadcast_running = 0;
}

/**
 * Initialize all world subsystems and run until a shutdown signal arrives.
 *
 * @param argc  Argument count; startup requires a configuration path in argv.
 * @param argv  Argument vector containing the required world configuration path.
 * @return      Zero after orderly shutdown, or one when initialization fails.
 */
int main(int argc, char** argv) {
    (void)argc;

    log_init();   // reads MMO_LOG_LEVEL; must run before any thread starts

    printf("=== WORLD SERVER ===\n");
    printf("PID: %d\n", getpid());

    init_data_paths();

    g_server_start_time = time(NULL);
    if (!session_init()) {
        fprintf(stderr, "Failed to initialize Redis session connection\n");
        return 1;
    }
    session_registry_init();
    connection_io_init();
    // packet_limiter_init runs after set_config below, so a world's .conf can
    // override the compiled budgets before the table is built.

    memset(&g_server, 0, sizeof(g_server));
    memset(&g_state, 0, sizeof(g_state));
    
    // Initialize g_server.running BEFORE setting up signals
    g_server.running = 1;
    
    // Ignore SIGPIPE — prevents crash when broadcast threads write to a client
    // socket that has been closed or reset.
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

    // Budgets come from the compiled world profile, with any per-world override
    // from the .conf applied on top. Must happen before the accept loop starts.
    {
        PacketLimitProfile profile;
        packet_limiter_apply_overrides(&profile, limit_profile_world(),
                                       &g_server.limits);
        packet_limiter_init(&profile);
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
    {
        // invalid shop content does not prevent startup
        int shop_problems = shop_validate();
        if (shop_problems == 0) printf("OK\n");
        else                    printf("OK (%d content problems — see log)\n", shop_problems);
    }

    printf("Loading zone definitions... ");
    fflush(stdout);
    if (zone_system_init(ZONES_PATH) >= 0) {
        printf("OK\n");
    } else {
        printf("SKIPPED (zones.json not found — zone notifications disabled)\n");
    }

    printf("Loading world collision map... ");
    fflush(stdout);
    if (world_collision_init(WORLD_DAT_PATH)) {
        printf("OK\n");
    } else {
        // Fatal on purpose. Without a collision map the server cannot tell open
        // ground from a wall, and starting anyway would mean running a world
        // where movement is unvalidated — which is worse than not running.
        printf("FAILED\n");
        fprintf(stderr, "Cannot start without a collision map at '%s'. "
                        "Movement validation depends on it.\n", WORLD_DAT_PATH);
        return 1;
    }

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
    
    // INIT EVENT LOOPS — connection count is no longer bounded by thread count.
    if (net_loop_start() != 0) {
        fprintf(stderr, "FAILED - could not start event loops\n");
        g_server.running = 0;
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
    
    // START WORLD BROADCAST THREAD (projectiles 30Hz, players 20Hz, npcs 10Hz)
    g_broadcast_running = 1;
    if (pthread_create(&g_broadcast_thread, NULL, world_broadcast_thread, NULL) != 0) {
        fprintf(stderr, "FAILED - World broadcast thread\n");
        g_broadcast_running = 0;
        g_combat_running = 0;
        g_server.running = 0;
        pthread_join(g_combat_thread, NULL);
        pthread_join(g_server.accept_thread, NULL);
        close(g_server.tcp_sockfd);
        playerdata_close();
        return 1;
    }
    
    printf("✓ All systems online - server ready\n");
    printf("  - Accept thread: Running\n");
    printf("  - Event loops: epoll (see [NET] line above for counts)\n");
    printf("  - Combat thread: 20Hz\n");
    printf("  - World broadcast thread: projectiles 30Hz, players 20Hz, npcs 10Hz\n");
    
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
    
    // Stop the broadcast and combat threads
    g_broadcast_running = 0;
    g_combat_running = 0;
    
    pthread_join(g_broadcast_thread, NULL);
    pthread_join(g_combat_thread, NULL);
    
    // Stop accept thread
    close(g_server.tcp_sockfd);
    pthread_join(g_server.accept_thread, NULL);

    // Drain and shut down the event loops and their blocking workers
    net_loop_stop();

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
    world_collision_shutdown();
    zone_system_cleanup();
    connection_io_shutdown();
    session_close();

    printf("World Server stopped\n");
    return 0;
}
