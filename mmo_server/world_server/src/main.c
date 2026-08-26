/**
 * @file
 * Initialize the world service and coordinate gameplay, networking, and broadcast threads.
 */
#include "types.h"
#include "log.h"
#include "packet_limiter.h"
#include "limit_profiles.h"
#include "metrics_server.h"
#include "net_notify.h"
#include "net_loop.h"
#include "session.h"
#include "realm_world_auth.h"
#include "tls.h"
#include "world_database_config.h"
#include "world_table.h"
#include "ip_allowlist.h"
#include "peer_addr.h"
#include "str_fixed.h"

#include "ability_def.h"
#include "ability_handler.h"
#include "class_stats.h"
#include "data_paths.h"
#include "connection_io.h"
#include "config.h"
#include "combat.h"
#include "dialogue_system.h"
#include "projectile.h"
#include "chat.h"
#include "loot.h"
#include "npc_ai.h"
#include "npc_snapshot.h"
#include "npc_spawns.h"
#include "npc_query.h"
#include "npc_world.h"
#include "routes.h"
#include "packet_handler.h"
#include "player_data.h"
#include "players_database.h"
#include "item_instance.h"
#include "session_registry.h"
#include "spatial_grid.h"
#include "broadcast_pool.h"
#include "broadcast_snapshot.h"
#include "tick_scheduler.h"
#include "party.h"
#include "quest_system.h"
#include "net_tuning.h"
#include "shop.h"
#include "shop_session.h"
#include "utils.h"
#include "world_collision.h"
#include "zone_system.h"

#include <sys/socket.h>
#include <arpa/inet.h>
#include <signal.h>
#include <stdatomic.h>
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
        LOG_ERROR("Runtime data path is too long: %s%s",
                  directory, relative_path);
        exit(EXIT_FAILURE);
    }

    memcpy(destination, directory, directory_length);
    memcpy(destination + directory_length, relative_path, relative_length + 1);
}

/* recv_exact_timeout() lived here, reading the realm's auth packet and its
 * heartbeats off a bare descriptor. That link is TLS now and reads go through
 * tls_recv_exact(), which also checks SSL_pending() before polling: one TLS
 * record can carry more than one heartbeat, and a poll-first loop would wait
 * out the full fifteen seconds on bytes already decrypted in the session and
 * then report the realm as timed out. */

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
    LOG_INFO("[PATHS] Data directory: %s/data/", exe);
}

static pthread_t g_combat_thread;

// One thread now carries every outbound stream — see the WORLD BROADCAST block
// below for why they were merged.
static pthread_t g_broadcast_thread;

/* Atomic for the same reason as g_server.running: the signal handler writes
 * these while the gameplay and broadcast threads read them, and `volatile`
 * orders nothing between threads. */
static _Atomic int g_combat_running = 0;
static _Atomic int g_broadcast_running = 0;

// Global for tracking world server uptime
time_t g_server_start_time = 0;

NPCWorld g_npc_world;

/* --- Realm handshake admission ------------------------------------------- */

/** Concurrently running realm handler threads; see g_server.realm_max_handlers. */
static _Atomic int g_realm_handlers = 0;

/** The world's TLS identity on the realm link, and the realms it accepts.
 *
 * Mutual: the realm checks this world's key against its own pin file, and this
 * checks the realm's certificate against g_realm_pins. Requiring a client
 * certificate is what makes the second half possible -- a peer that sends none
 * has nothing to pin, and would otherwise reach the server-key comparison on
 * the strength of having completed a handshake.
 *
 * The link is worth this because of what crosses it: the shared server auth
 * key, which is the credential a realm proves itself with, and which a world
 * accepts as licence to be listed and to admit the tickets that realm mints.
 */
static SSL_CTX*   g_realm_tls_ctx = NULL;
static CertPinSet g_realm_pins;

/** One accepted realm connection handed to its detached handler thread. */
typedef struct {
    int  fd;
    char peer[PEER_ADDR_MAXLEN];
} RealmHandlerArg;


/** Send one realm auth refusal over the session. */
static void realm_auth_refuse(SSL* ssl, const char* message) {
    RealmAuthAckPacket ack = {0};
    ack.header.type = PACKET_REALM_AUTH_ACK;
    ack.header.player_id = 0;
    ack.header.payload_size = 0;
    ack.success = 0;
    STR_COPY_FIELD(ack.message, message);

    tls_send_exact(ssl, &ack, sizeof(ack), 5000);
}

/** Load the world's realm-link identity and the realm keys it will accept.
 *
 * @return 1 when the realm listener can be opened, otherwise 0.
 */
static int realm_link_tls_init(const char* cert_path, const char* key_path,
                               const char* realm_pin_path) {
    char reason[256];
    cert_pin_reset(&g_realm_pins);

    if (!cert_pin_load_file(&g_realm_pins, realm_pin_path, reason, sizeof(reason))) {
        LOG_ERROR("No usable realm public-key pins in %s: %s", realm_pin_path, reason);
        LOG_INFO("This world will refuse every realm handshake without them.");
        LOG_INFO("Run setup/setup.sh, which generates and cross-installs them.");
        return 0;
    }

    g_realm_tls_ctx = tls_server_init_mutual(cert_path, key_path);
    if (!g_realm_tls_ctx) {
        LOG_ERROR("Could not load this world's certificate (%s)", cert_path);
        cert_pin_reset(&g_realm_pins);
        return 0;
    }

    LOG_INFO("Realm link TLS ready (%d realm key%s pinned)",
             g_realm_pins.count, g_realm_pins.count == 1 ? "" : "s");
    return 1;
}

/** Release what realm_link_tls_init() loaded. */
static void realm_link_tls_cleanup(void) {
    if (g_realm_tls_ctx) {
        SSL_CTX_free(g_realm_tls_ctx);
        g_realm_tls_ctx = NULL;
    }
    cert_pin_reset(&g_realm_pins);
}

/**
 * Authenticate one realm connection and answer its world-status heartbeats.
 *
 * The function owns and frees the heap-allocated argument, releases its handler
 * slot, then closes the realm descriptor before returning.
 *
 * @return      Always NULL.
 */
void* realm_handler_thread(void* arg) {
    RealmHandlerArg* handoff = (RealmHandlerArg*)arg;
    int realm_fd = handoff->fd;

    char peer[PEER_ADDR_MAXLEN];
    STR_COPY_FIELD(peer, handoff->peer);
    free(handoff);

    LOG_INFO("Realm server connection handler started: fd %d (%s)", realm_fd, peer);

    /* The handshake comes before anything else, and the pin check is inside it.
     * The peer has already passed the address allowlist, which says where it
     * came from; this says who it is. A peer that fails either never reaches
     * the server-key comparison below -- and the server key is the thing worth
     * protecting here, because it is the same secret for every realm. */
    SSL* ssl = tls_accept_pinned(g_realm_tls_ctx, realm_fd, &g_realm_pins,
                                 peer, 10000);
    if (!ssl) {
        LOG_WARN("Realm TLS handshake from %s failed or was not pinned", peer);
        close(realm_fd);
        atomic_fetch_sub(&g_realm_handlers, 1);
        return NULL;
    }

    // First packet should be REALM_AUTH
    RealmAuthPacket auth;
    struct pollfd pfd = {.fd = realm_fd, .events = POLLIN};

    if (!tls_recv_exact(ssl, &auth, sizeof(auth), 10000)) {
        LOG_ERROR("Failed to receive realm auth from %s", peer);
        tls_close(ssl);
        close(realm_fd);
        atomic_fetch_sub(&g_realm_handlers, 1);
        return NULL;
    }

    if (auth.header.type != PACKET_REALM_AUTH) {
        LOG_ERROR("Invalid packet type from %s, expected REALM_AUTH", peer);
        tls_close(ssl);
        close(realm_fd);
        atomic_fetch_sub(&g_realm_handlers, 1);
        return NULL;
    }

    /* Terminate both fixed-width fields before anything reads them as strings.
     *
     * server_key and realm_name arrive straight off the wire as char[128] and
     * char[32] with no guarantee of a NUL anywhere inside them. strcmp() and
     * "%s" both read until they find one, so a peer that fills either field
     * completely walked the comparison and the log line off the end of the
     * packet and into the rest of this thread's stack -- reachable before any
     * authentication, by anyone who could open the port. */
    char provided_key[sizeof(auth.server_key) + 1];
    char realm_name[sizeof(auth.realm_name) + 1];
    str_copy_fixed(provided_key, sizeof(provided_key), auth.server_key);
    str_copy_fixed(realm_name, sizeof(realm_name), auth.realm_name);

    const char* world_name = g_server.server_name;

    /* Accepts the current key or the one it replaced, and compares in constant
     * time. The overlap matters: keys are rotated on a schedule and the realm
     * caches one for a whole probe cycle, so without it a rotation landing
     * between the realm's read and this check takes the world off the world
     * list until the next cycle -- and a missed rotation takes it off
     * permanently, reported only as "Server key unavailable". */
    if (!validate_server_auth_key(provided_key, world_name)) {
        LOG_WARN("Invalid realm server key from '%s' at %s", realm_name, peer);
        realm_auth_refuse(ssl, "Invalid server key");
        tls_close(ssl);
        close(realm_fd);
        atomic_fetch_sub(&g_realm_handlers, 1);
        return NULL;
    }

    // Send success response
    RealmAuthAckPacket ack = {0};
    ack.header.type = PACKET_REALM_AUTH_ACK;
    ack.header.player_id = 0;
    ack.header.payload_size = 0;
    ack.success = 1;
    STR_COPY_FIELD(ack.message, "Authenticated successfully");

    if (!tls_send_exact(ssl, &ack, sizeof(ack), 5000)) {
        LOG_ERROR("Failed to send auth ack");
        tls_close(ssl);
        close(realm_fd);
        atomic_fetch_sub(&g_realm_handlers, 1);
        return NULL;
    }

    LOG_INFO("Realm server '%s' at %s authenticated over TLS", realm_name, peer);

    // Handle heartbeats
    pfd.fd = realm_fd;
    pfd.events = POLLIN;

    while (g_server.running) {
        /* Poll only when the session has nothing buffered. A TLS record can
         * carry more than one heartbeat, and poll() reports the descriptor
         * rather than the record -- so polling unconditionally would sit out
         * the full fifteen seconds on a heartbeat that had already arrived and
         * then declare the realm timed out. */
        if (!tls_pending(ssl)) {
            int ret = poll(&pfd, 1, 15000);

            if (ret < 0) {
                if (errno == EINTR) continue;
                LOG_INFO("Poll error on realm connection");
                break;
            }

            if (ret == 0) {
                LOG_INFO("Realm server heartbeat timeout");
                break;
            }

            if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
                LOG_INFO("Realm connection error");
                break;
            }
            if (!(pfd.revents & POLLIN)) continue;
        }

        {
            WorldHeartbeatPacket hb;
            if (!tls_recv_exact(ssl, &hb, sizeof(hb), 15000)) {
                LOG_INFO("Realm server disconnected");
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

                if (!tls_send_exact(ssl, &status, sizeof(status), 5000)) {
                    LOG_ERROR("Failed to send status to realm server");
                    break;
                }
            } else {
                LOG_INFO("Unexpected packet type %d from realm server", hb.header.type);
            }
        }
    }

    tls_close(ssl);
    close(realm_fd);
    atomic_fetch_sub(&g_realm_handlers, 1);
    LOG_INFO("Realm server handler exiting (%s)", peer);
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

        LOG_DEBUG("New connection from %s:%d",
                  inet_ntoa(client_addr.sin_addr),
                  ntohs(client_addr.sin_port));

        /* Straight to an event loop. Nothing is peeked at and nothing is
         * classified: this listener carries one protocol.
         *
         * What used to happen here was a 3-second poll and an MSG_PEEK on
         * every accepted connection to see whether the first byte was
         * PACKET_REALM_AUTH -- so the realm's privileged handshake lived on
         * the same port as the game, every player paid a syscall pair to prove
         * they were not a realm, and a client that sent nothing held the
         * single accept thread for three seconds before being dropped. The
         * realm has its own listener now; see realm_accept_thread_func().
         *
         * The socket is pinned to loop (fd % N) for its whole life, so exactly
         * one thread ever services it. */
        net_loop_submit(client_fd);
    }

    return NULL;
}

/**
 * Accept realm-to-world connections on the private realm listener.
 *
 * Everything reaching this socket is claiming to be a realm, so the checks are
 * the admission policy for that claim rather than a classification step: the
 * source address must be in `realm_allow`, and the number of handlers already
 * running must be under the configured cap. Both happen before any allocation
 * and before a thread exists, because each handler blocks for up to twenty
 * seconds before authentication decides anything.
 *
 * @return Always NULL after the server stops.
 */
void* realm_accept_thread_func(void* arg) {
    (void)arg;

    struct pollfd pfd = {.fd = g_server.realm_sockfd, .events = POLLIN};

    while (g_server.running) {
        int ret = poll(&pfd, 1, 1000);
        if (ret <= 0) continue;

        struct sockaddr_in peer_addr;
        socklen_t addr_len = sizeof(peer_addr);

        int realm_fd = accept(g_server.realm_sockfd,
                              (struct sockaddr*)&peer_addr, &addr_len);
        if (realm_fd < 0) continue;

        if (!ip_allowlist_contains(&g_server.realm_allow,
                                   (struct sockaddr*)&peer_addr)) {
            LOG_ERROR("Refused realm handshake from %s (not in realm_allow)",
                      inet_ntoa(peer_addr.sin_addr));
            close(realm_fd);
            continue;
        }

        int in_flight = atomic_fetch_add(&g_realm_handlers, 1) + 1;
        if (in_flight > g_server.realm_max_handlers) {
            atomic_fetch_sub(&g_realm_handlers, 1);
            LOG_ERROR("Refused realm handshake from %s (%d handlers already running, cap %d)",
                      inet_ntoa(peer_addr.sin_addr), in_flight - 1,
                      g_server.realm_max_handlers);
            close(realm_fd);
            continue;
        }

        RealmHandlerArg* handoff = malloc(sizeof(*handoff));
        if (!handoff) {
            atomic_fetch_sub(&g_realm_handlers, 1);
            LOG_ERROR("Out of memory accepting a realm connection");
            close(realm_fd);
            continue;
        }

        handoff->fd = realm_fd;
        peer_addr_text(realm_fd, handoff->peer, sizeof(handoff->peer));
        LOG_INFO("Realm server connection from %s", handoff->peer);

        pthread_t thread;
        if (pthread_create(&thread, NULL, realm_handler_thread, handoff) != 0) {
            atomic_fetch_sub(&g_realm_handlers, 1);
            LOG_ERROR("Failed to create realm handler thread");
            close(realm_fd);
            free(handoff);
        } else {
            pthread_detach(thread);
        }
    }

    return NULL;
}

/** Number of measured phases in each gameplay tick. */
#define TICK_PHASE_COUNT 6

/** Milliseconds one gameplay tick may take before the world falls behind 20Hz.
 *
 * The same 50ms the tick report has always printed as "the budget"; named here
 * so the health check and the log line cannot disagree about it. */
#define TICK_BUDGET_MS 50.0
static const char* g_phase_name[TICK_PHASE_COUNT] = {
    "snapshot", "combat", "ability", "projectile", "npc_ai", "loot"
};
static double g_phase_total_ms[TICK_PHASE_COUNT];
static double g_phase_worst_ms[TICK_PHASE_COUNT];
static long   g_phase_samples;

/* The last completed timing window, kept for the metrics endpoint.
 *
 * The accumulators above are cleared every ten seconds by tick_phase_report(),
 * so a scrape landing just after one would see almost nothing. These hold the
 * window that just closed, which is a stable value to read at any moment.
 *
 * Written by the gameplay thread and read by the metrics thread. Plain doubles
 * rather than atomics on purpose: a torn read produces a slightly wrong number
 * on one scrape, and paying for atomics on a 20Hz gameplay path to avoid that
 * would be the wrong trade. Nothing decides anything from them but a graph.
 */
static double g_published_phase_mean[TICK_PHASE_COUNT];
static double g_published_phase_worst[TICK_PHASE_COUNT];
static double g_published_tick_mean_ms;
static double g_published_tick_worst_ms;

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
                 " | total mean %.2fms of %.0fms budget",
                 mean_total, TICK_BUDGET_MS);

    LOG_INFO("%s", line);
    LOG_DEBUG("[TICK] figures are mean/worst milliseconds per phase");

    /* Publish before clearing, so the endpoint always has a whole window. */
    double worst_total = 0.0;
    for (int p = 0; p < TICK_PHASE_COUNT; p++) {
        g_published_phase_mean[p]  = g_phase_total_ms[p] / (double)g_phase_samples;
        g_published_phase_worst[p] = g_phase_worst_ms[p];
        worst_total += g_phase_worst_ms[p];
    }
    g_published_tick_mean_ms  = mean_total;
    g_published_tick_worst_ms = worst_total;

    memset(g_phase_total_ms, 0, sizeof(g_phase_total_ms));
    memset(g_phase_worst_ms, 0, sizeof(g_phase_worst_ms));
    g_phase_samples = 0;
}

/* --- The metrics endpoint ------------------------------------------------ */

/** Write this world's numbers in Prometheus text format. */
static size_t world_metrics(char* out, size_t out_size, void* user) {
    (void)user;
    size_t used = 0;

    metrics_write(out, out_size, &used, "mmo_world_players",
                  "Players currently in this world", "gauge",
                  (double)atomic_load(&g_state.current_players));
    metrics_write(out, out_size, &used, "mmo_world_max_players",
                  "Configured capacity", "gauge",
                  (double)g_server.max_players);
    metrics_write(out, out_size, &used, "mmo_world_uptime_seconds",
                  "Seconds since this world started", "counter",
                  (double)(time(NULL) - g_server_start_time));
    metrics_write(out, out_size, &used, "mmo_world_id",
                  "This world's identifier in worlds.conf", "gauge",
                  (double)g_server.world_id);

    /* Tick health. The budget is 50ms; a mean approaching it means the world
     * is about to stop keeping 20Hz, which is the single most useful early
     * warning this process can emit. */
    metrics_write(out, out_size, &used, "mmo_world_tick_mean_ms",
                  "Mean total gameplay tick time over the last window, against a 50ms budget",
                  "gauge", g_published_tick_mean_ms);
    metrics_write(out, out_size, &used, "mmo_world_tick_worst_ms",
                  "Worst total gameplay tick time over the last window", "gauge",
                  g_published_tick_worst_ms);

    for (int p = 0; p < TICK_PHASE_COUNT; p++) {
        char name[96], help[160];
        snprintf(name, sizeof(name), "mmo_world_tick_phase_mean_ms_%s", g_phase_name[p]);
        snprintf(help, sizeof(help), "Mean time in the %s tick phase", g_phase_name[p]);
        metrics_write(out, out_size, &used, name, help, "gauge",
                      g_published_phase_mean[p]);

        snprintf(name, sizeof(name), "mmo_world_tick_phase_worst_ms_%s", g_phase_name[p]);
        snprintf(help, sizeof(help), "Worst time in the %s tick phase", g_phase_name[p]);
        metrics_write(out, out_size, &used, name, help, "gauge",
                      g_published_phase_worst[p]);
    }

    metrics_write(out, out_size, &used, "mmo_world_npcs_alive",
                  "NPCs occupying a pool slot", "gauge",
                  (double)npc_world_count(&g_npc_world));
    metrics_write(out, out_size, &used, "mmo_world_npc_capacity",
                  "Configured NPC pool size", "gauge",
                  (double)npc_world_capacity(&g_npc_world));

    /* Queue depths. Both are backpressure signals: a chat queue that stays
     * deep means the dispatcher is behind the players, and a worker queue that
     * stays deep means the blocking work -- saves, logins -- is behind the
     * network. */
    metrics_write(out, out_size, &used, "mmo_world_chat_queue_depth",
                  "Chat messages waiting to be delivered", "gauge",
                  (double)chat_queue_depth());
    metrics_write(out, out_size, &used, "mmo_world_chat_dropped_total",
                  "Chat messages dropped for backpressure since startup", "counter",
                  (double)chat_dropped_count());

    int job_capacity = 0;
    int job_depth = net_loop_job_depth(&job_capacity);
    metrics_write(out, out_size, &used, "mmo_world_worker_queue_depth",
                  "Blocking jobs waiting for a worker", "gauge", (double)job_depth);
    metrics_write(out, out_size, &used, "mmo_world_worker_queue_capacity",
                  "Blocking job queue capacity", "gauge", (double)job_capacity);
    metrics_write(out, out_size, &used, "mmo_world_connections",
                  "Open client connections, authenticated or not", "gauge",
                  (double)net_loop_connection_count());

    int pool_in_use = 0, pool_size = 0, pool_waiters = 0;
    character_database_pool_stats(&pool_in_use, &pool_size, &pool_waiters);
    metrics_write(out, out_size, &used, "mmo_world_db_pool_in_use",
                  "Database connections checked out", "gauge", (double)pool_in_use);
    metrics_write(out, out_size, &used, "mmo_world_db_pool_size",
                  "Database connection pool size", "gauge", (double)pool_size);
    metrics_write(out, out_size, &used, "mmo_world_db_pool_waiters",
                  "Threads waiting for a database connection", "gauge",
                  (double)pool_waiters);

    metrics_write(out, out_size, &used, "mmo_world_shops_open",
                  "Characters with a shop open", "gauge",
                  (double)shop_session_count());

    /* What the packet limiter has refused. Until these existed the limiter's
     * only output was a rate-limited log line, so a world under a packet flood
     * looked exactly like a quiet one to anything scraping it. */
    unsigned long long lim_allowed = 0, lim_dropped = 0, lim_kicked = 0;
    packet_limiter_totals(&lim_allowed, &lim_dropped, &lim_kicked);
    metrics_write(out, out_size, &used, "mmo_world_packets_allowed_total",
                  "Packets accepted by the rate limiter since startup", "counter",
                  (double)lim_allowed);
    metrics_write(out, out_size, &used, "mmo_world_packets_dropped_total",
                  "Packets dropped for exceeding a packet budget since startup", "counter",
                  (double)lim_dropped);
    metrics_write(out, out_size, &used, "mmo_world_limiter_kicks_total",
                  "Connections closed for sustained packet-budget abuse since startup",
                  "counter", (double)lim_kicked);

    return used;
}

/**
 * Decide whether this world is healthy enough to be sent players.
 *
 * Three things make it not: the gameplay loop has stopped, the tick is over
 * budget so the world is no longer running at 20Hz, or the database pool is
 * saturated with threads queued behind it. Each is a reason a load balancer
 * should route elsewhere and a restart policy should look closer.
 *
 * Being full is deliberately not one of them. A full world is working
 * correctly; the realm already reports capacity and steers players away.
 */
static int world_health(char* reason, size_t reason_size, void* user) {
    (void)user;

    if (!g_server.running) {
        snprintf(reason, reason_size, "shutting down");
        return 0;
    }
    if (!atomic_load(&g_combat_running)) {
        snprintf(reason, reason_size, "the gameplay thread is not running");
        return 0;
    }
    if (g_published_tick_mean_ms > TICK_BUDGET_MS) {
        snprintf(reason, reason_size,
                 "gameplay tick averaging %.1fms against a %.0fms budget",
                 g_published_tick_mean_ms, TICK_BUDGET_MS);
        return 0;
    }

    int in_use = 0, size = 0, waiters = 0;
    character_database_pool_stats(&in_use, &size, &waiters);
    if (size > 0 && in_use >= size && waiters > 0) {
        snprintf(reason, reason_size,
                 "database pool saturated (%d/%d in use, %d waiting)",
                 in_use, size, waiters);
        return 0;
    }

    return 1;
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

    // The NPC counterpart, for the same reason: every phase below used to
    // rescan the whole NPC pool under one lock. See npc_snapshot.h.
    static NPCTickSnapshot npc_snapshot;
    if (!npc_snapshot_init(&npc_snapshot, &g_npc_world)) {
        LOG_ERROR("[TICK] gameplay thread cannot start without an NPC grid");
        tick_snapshot_free(&snapshot);
        return NULL;
    }

    double phase_ms[TICK_PHASE_COUNT];
    double last_report = mono_ms();

    LOG_INFO("Combat update thread started (20Hz)");

    while (g_combat_running && g_server.running) {
        double t0 = mono_ms(), t1;

        // Both snapshots are rebuilt together, at the top of the tick, so every
        // phase below sees one consistent view of players and NPCs.
        tick_snapshot_build(&snapshot);
        npc_snapshot_build(&npc_snapshot, &g_npc_world);
        /* Republish the shared index the packet threads read, from the same
         * sample instant. Without it, attack packets fall back to scanning the
         * whole pool on a network loop thread. */
        npc_query_publish(&g_npc_world);
                                              t1 = mono_ms(); phase_ms[0] = t1 - t0; t0 = t1;
        combat_tick(&g_npc_world, &npc_snapshot);          t1 = mono_ms(); phase_ms[1] = t1 - t0; t0 = t1;
        ability_tick(&g_npc_world, &snapshot, &npc_snapshot, DELTA_TIME); t1 = mono_ms(); phase_ms[2] = t1 - t0; t0 = t1;
        projectile_tick(&g_npc_world, &snapshot, &npc_snapshot, DELTA_TIME);
                                              t1 = mono_ms(); phase_ms[3] = t1 - t0; t0 = t1;
        npc_ai_tick(&g_npc_world, &snapshot, DELTA_TIME);  t1 = mono_ms(); phase_ms[4] = t1 - t0; t0 = t1;
        loot_tick(&snapshot);                 t1 = mono_ms(); phase_ms[5] = t1 - t0;

        /* Off the phase table on purpose: it walks one entry per character
         * with a shop open, which is a handful, and giving it a phase would
         * cost more in clock reads than the sweep itself. */
        shop_session_tick();

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
            LOG_INFO("[WARNING] Combat thread lagging by %ldms", drift_ns / 1000000);
        }
    }

    tick_phase_report();          // final numbers before the counters go away
    npc_snapshot_free(&npc_snapshot);
    tick_snapshot_free(&snapshot);

    LOG_INFO("Combat update thread exiting");
    return NULL;
}


/** Player-to-player interest radius in world pixels. */
#define PLAYER_VIEW_RADIUS 800.0f

/** Player-to-NPC interest radius in world pixels. */
#define NPC_VIEW_RADIUS 2000.0f

/** Bucket count for the party-dedup set; a power of two above MAX_PLAYERS. */
#define PARTY_SET_BUCKETS 2048
_Static_assert(PARTY_SET_BUCKETS > MAX_PLAYERS,
               "the party set must have room for one bucket per broadcast player");

/** Own one shared player snapshot and broadcast-task scratch storage. */
typedef struct {
    BroadcastPlayer   players[MAX_PLAYERS];
    SpatialPoint      player_points[MAX_PLAYERS];
    BroadcastSnapshot snapshot;

    // NPC scratch. Only the NPC task reads this, so it is filled there rather
    // than in prepare — sampling every NPC on every 30Hz pass to feed a 10Hz
    // stream would be two thirds wasted.
    //
    // Heap-allocated and sized from the pool's capacity, which is a runtime
    // value now rather than a compile-time one.
    NPCPositionData*  npc_wire;
    SpatialPoint*     npc_points;
    int               npc_capacity;

    /** One NPC grid per shard; SpatialGrid keeps query scratch and is not shared. */
    SpatialGrid*      npc_grids[BROADCAST_POOL_MAX_SHARDS];
    int               shard_count;

    /** Dedup set for the party stream; see task_broadcast_parties(). */
    uint32_t          party_seen[PARTY_SET_BUCKETS];
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
        snap->race_id = (uint8_t)active_players[i].race_id;
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
}

/** Broadcast nearby player state for one shard of the pass. */
static void shard_broadcast_players(void* ctx, int shard, int shard_count) {
    BroadcastContext* bc = ctx;
    const BroadcastSnapshot* snap = &bc->snapshot;

    for (int i = 0; i < snap->count; i++) {
        // Sharded on the descriptor, matching how net_loop pins connections, so
        // this shard writes only to sockets its own loop owns.
        if (shard_count > 1 && (snap->players[i].client_fd % shard_count) != shard)
            continue;

        PlayerPositionBroadcastPacket pkt;
        memset(&pkt, 0, sizeof(pkt));
        pkt.header.type = PACKET_PLAYER_POSITIONS;
        pkt.header.player_id = htonl(snap->players[i].character_id);
        pkt.count = 0;

        // One extra slot: the viewer is in the grid too and comes back as its
        // own nearest result, so without it a full crowd would cost this
        // player one visible neighbour.
        int nearby[MAX_NEARBY_PLAYERS + 1];
        int nearby_count = spatial_grid_query(snap->grids[shard],
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
            np->player_class = snap->players[j].race_id;
            np->player_race  = snap->players[j].race_id;
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

}

/** Broadcast one party update per represented party.
 *
 * Not sharded: party_broadcast_update() sends to a party's whole roster, which
 * spans descriptors and therefore shards. It runs once, on the pass thread,
 * after the sharded player fan-out has finished.
 */
static void task_broadcast_parties(void* ctx) {
    BroadcastContext* bc = ctx;
    const BroadcastSnapshot* snap = &bc->snapshot;

    /* Dedup through an open-addressed set rather than a linear scan of what has
     * already been seen. The scan was O(n^2) over every broadcast player, every
     * tick — at 1000 players in parties that is half a million comparisons a
     * tick to produce at most a few hundred sends. */
    uint32_t* seen = bc->party_seen;
    memset(seen, 0, sizeof(bc->party_seen));   // 0 is "empty"

    for (int i = 0; i < snap->count; i++) {
        uint32_t pid = snap->players[i].party_id;
        if (pid == 0) continue;

        uint32_t bucket = (pid * 2654435761u) & (PARTY_SET_BUCKETS - 1);
        int already = 0;

        // Terminates: the table has more buckets than there can be players.
        while (seen[bucket] != 0) {
            if (seen[bucket] == pid) { already = 1; break; }
            bucket = (bucket + 1) & (PARTY_SET_BUCKETS - 1);
        }
        if (already) continue;

        seen[bucket] = pid;
        party_broadcast_update(pid);
    }
}

/** Fan the player stream out across the pool, then send party updates once. */
static void task_broadcast_players(void* ctx) {
    BroadcastContext* bc = ctx;

    /* One grid per shard over the same points. Built here rather than in
     * broadcast_prepare() because prepare runs every pass (30Hz) while this
     * stream only goes out at 20Hz — indexing the players a third more often
     * than anything reads the result is wasted work. */
    for (int g = 0; g < bc->snapshot.shard_count; g++)
        spatial_grid_build(bc->snapshot.grids[g], bc->player_points, bc->snapshot.count);

    broadcast_pool_run(shard_broadcast_players, ctx);
    task_broadcast_parties(ctx);
}

/** Broadcast nearest in-range NPCs to one shard of the pass. */
static void shard_broadcast_npcs(void* ctx, int shard, int shard_count) {
    BroadcastContext* bc = ctx;
    const BroadcastSnapshot* snap = &bc->snapshot;

    for (int i = 0; i < snap->count; i++) {
        if (shard_count > 1 && (snap->players[i].client_fd % shard_count) != shard)
            continue;

        NPCPositionPacket pkt;
        memset(&pkt, 0, sizeof(pkt));
        pkt.header.type = PACKET_NPC_POSITIONS;
        pkt.header.player_id = htonl(snap->players[i].character_id);
        pkt.header.payload_size = 0;
        pkt.npc_count = 0;

        int nearby[MAX_NPCS_PER_PACKET];
        int nearby_count = spatial_grid_query(bc->npc_grids[shard],
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

/** Sample NPCs once, then fan their stream out across the pool. */
static void task_broadcast_npcs(void* ctx) {
    BroadcastContext* bc = ctx;

    /* Nobody to send to. Sampling the pool first and discovering that
     * afterwards is what this pass used to do -- ten times a second, holding
     * the pool read lock and every NPC's mutex in turn, to build a stream with
     * no recipients. An empty world now costs one comparison. */
    if (bc->snapshot.count == 0) return;

    // Sample the NPCs once for the whole pass. The version before last took the
    // NPC lock once per player per tick; this takes the pool read lock once and
    // one slot lock at a time.

    int npc_count = 0;

    // Shares the pool read lock with the gameplay thread and takes one NPC's
    // mutex at a time, so a broadcast pass no longer excludes gameplay.
    npc_world_read_begin(&g_npc_world);

    /* And it walks the NPCs that exist rather than the pool they live in: a
     * world configured with room for four thousand NPCs and running two
     * hundred used to pay for four thousand, ten times a second. */
    int live_count = 0;
    const int* live = npc_world_live_slots(&g_npc_world, &live_count);

    for (int i = 0; i < live_count && npc_count < bc->npc_capacity; i++) {
        int n = live[i];
        NPCEntity* npc = npc_world_slot(&g_npc_world, n);
        if (!npc || npc->id == 0) continue;   // unlocked pre-filter only

        npc_world_slot_lock(&g_npc_world, n);

        if (npc->id != 0) {
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

        npc_world_slot_unlock(&g_npc_world, n);
    }

    npc_world_read_end(&g_npc_world);

    if (npc_count == 0) return;

    /* One grid per shard, all built from the same points.
     *
     * SpatialGrid keeps its query scratch inside itself and is documented as
     * single-thread-only, so shards cannot share one. Building N copies of an
     * index over a few hundred NPCs is far cheaper than serialising N shards'
     * worth of queries and sends behind a shared one. */
    for (int g = 0; g < bc->shard_count; g++)
        spatial_grid_build(bc->npc_grids[g], bc->npc_points, npc_count);

    broadcast_pool_run(shard_broadcast_npcs, ctx);
}

/** Send the projectile stream for one shard of the pass. */
static void shard_broadcast_projectiles(void* ctx, int shard, int shard_count) {
    BroadcastContext* bc = ctx;
    projectile_broadcast(&bc->snapshot, shard, shard_count);
}

static void task_broadcast_projectiles(void* ctx) {
    broadcast_pool_run(shard_broadcast_projectiles, ctx);
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

    bc->npc_capacity = npc_world_capacity(&g_npc_world);
    bc->npc_wire     = calloc((size_t)bc->npc_capacity, sizeof(NPCPositionData));
    bc->npc_points   = calloc((size_t)bc->npc_capacity, sizeof(SpatialPoint));

    /* Shard on the loop count so a connection's outbound traffic stays on the
     * thread whose epoll loop already owns that descriptor. */
    broadcast_pool_start(net_loop_count());
    bc->shard_count          = broadcast_pool_shards();
    bc->snapshot.shard_count = bc->shard_count;

    /* Both streams need one grid per shard, for the same reason: a shard queries
     * concurrently with every other shard, and a grid's query scratch belongs to
     * one thread. */
    int grids_ok = 1;
    for (int g = 0; g < bc->shard_count; g++) {
        bc->snapshot.grids[g] = spatial_grid_create(world_w, world_h,
                                                    SPATIAL_GRID_DEFAULT_CELL,
                                                    MAX_PLAYERS);
        bc->npc_grids[g]      = spatial_grid_create(world_w, world_h,
                                                    SPATIAL_GRID_DEFAULT_CELL,
                                                    bc->npc_capacity);
        if (!bc->snapshot.grids[g] || !bc->npc_grids[g]) grids_ok = 0;
    }

    if (!grids_ok || !bc->npc_wire || !bc->npc_points) {
        LOG_ERROR("[BROADCAST] could not allocate the interest grids");
        for (int g = 0; g < bc->shard_count; g++) {
            spatial_grid_destroy(bc->snapshot.grids[g]);
            spatial_grid_destroy(bc->npc_grids[g]);
        }
        broadcast_pool_stop();
        free(bc->npc_wire);
        free(bc->npc_points);
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
        spatial_grid_dimensions(bc->snapshot.grids[0], &cols, &rows);
        LOG_INFO("[BROADCAST] thread started — projectiles 30Hz, players 20Hz, "
                 "npcs 10Hz, %dx%d interest grid @ %.0fpx",
                 cols, rows, SPATIAL_GRID_DEFAULT_CELL);
    }

    // The flag is raised by main() before this thread is created, not here:
    // setting it here would let a shutdown signal that arrives during startup
    // be overwritten, leaving the loop running with nothing to stop it.
    tick_scheduler_run(&scheduler, &g_broadcast_running);

    tick_scheduler_report(&scheduler);

    broadcast_pool_stop();

    for (int g = 0; g < bc->shard_count; g++) {
        spatial_grid_destroy(bc->snapshot.grids[g]);
        spatial_grid_destroy(bc->npc_grids[g]);
    }
    free(bc->npc_wire);
    free(bc->npc_points);
    free(bc);

    LOG_INFO("[BROADCAST] thread exiting");
    return NULL;
}

/**
 * Tell every connected client that the world is going down.
 *
 * Best-effort and non-blocking: connection_io_send() queues on a socket that
 * cannot take the bytes now, and the loops are still running here, so a slow
 * client does not hold up the shutdown. A client that misses it falls back to
 * the ping timeout it would have hit anyway.
 */
static void broadcast_shutdown_notice(void) {
    extern ActivePlayer active_players[];

    uint8_t packet[sizeof(DisconnectPacket)];
    size_t n = net_build_disconnect(packet, sizeof(packet),
                                    DISCONNECT_REASON_SHUTDOWN, NULL);
    if (!n) return;

    int sent = 0;
    player_registry_rdlock();
    int online_count = 0;
    const int* online = player_active_list_locked(&online_count);
    for (int i = 0; i < online_count; i++) {
        int slot = online[i];
        if (!active_players[slot].is_loaded) continue;
        int fd = active_players[slot].client_fd;
        if (fd < 0) continue;
        connection_io_send(fd, packet, n);
        sent++;
    }
    player_registry_unlock();

    if (sent > 0) LOG_INFO("Told %d client(s) the server is shutting down", sent);
}

/** Record which signal asked for the shutdown, for the main thread to report. */
static volatile sig_atomic_t g_shutdown_signal = 0;

/** Request shutdown of the world server and its gameplay threads.
 *
 * Flag writes only. This used to call LOG_INFO(), which reaches vsnprintf(),
 * a mutex and write(2) -- none of them async-signal-safe. A SIGINT arriving
 * while another thread held the log's lock deadlocked the handler, and one
 * arriving inside malloc() during formatting could corrupt the heap. The main
 * loop below reports the signal once it wakes.
 *
 * sig_atomic_t for the signal number and _Atomic ints for the run flags: both
 * are safe to write from a handler, and the flags are already read by the
 * gameplay threads.
 */
void signal_handler(int signum) {
    g_shutdown_signal = signum;
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

    LOG_INFO("=== WORLD SERVER ===");
    LOG_INFO("PID: %d", getpid());

    init_data_paths();

    g_server_start_time = time(NULL);
    if (!session_init()) {
        LOG_ERROR("Failed to initialize Redis session connection");
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

    LOG_INFO("[SIGNAL] Signal handlers installed");

    // Parse config file
    if(argv && argv[1]) {
        if(!set_config(argv[1])) {
            LOG_INFO("Usage: ./world_server <server>.conf");
            exit(1);
        }
    } else {
        LOG_INFO("Usage: ./world_server <server>.conf");
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
        LOG_ERROR("FAILED - Item system initialization");
        return 1;
    }

    /* The race registry must exist before abilities load: an ability names the race
     * that owns it by key, and one loaded first would resolve to no race at all. The
     * attack profiles name races the same way. */
    char races_path[512], progression_path[512];
    data_path_resolve(races_path, sizeof(races_path), "/data/races.json");
    data_path_resolve(progression_path, sizeof(progression_path), "/data/progression.json");
    if (!class_stats_init(races_path, progression_path)) {
        LOG_ERROR("FATAL: no races loaded from %s — no character can be run", races_path);
        return 1;
    }

    if (!abilities_init(ABILITIES_PATH)) {
        LOG_ERROR("FAILED - Ability system initialization");
        return 1;
    }

    combat_profiles_load(ATTACK_PROFILES_PATH);

    ability_handler_init();
    projectile_init();

    if (!loot_init(DATA_PATH)) {
        LOG_ERROR("FAILED - Loot system initialization");
        return 1;
    }

    if (!npc_ai_init(NPC_TYPES_PATH)) {
        LOG_ERROR("FAILED - NPC AI system initialization");
        return 1;
    }

    /* One record per subsystem, stated after the fact.
     *
     * These used to be "Loading X... " with no newline, an fflush, and a bare
     * "OK" once it finished -- a progress line assembled from two writes. That
     * shape cannot survive a log: a record is a whole line, it carries a
     * timestamp and a level, and it may be interleaved with another thread's.
     * Saying what happened once it has happened costs nothing and is what can
     * actually be read back out of a file afterwards. */
    if (!dialogue_system_init(DIALOGUES_PATH)) {
        LOG_ERROR("FAILED - Dialogue system initialization");
        return 1;
    }
    LOG_INFO("Dialogue system loaded (%d dialogues)", dialogues_get_count());

    quest_storage_set_dir(QUEST_SAVE_DIR);
    quest_registry_load(QUESTS_PATH);
    LOG_INFO("Quest system loaded");

    shop_init(SHOPS_PATH);
    {
        // invalid shop content does not prevent startup
        int shop_problems = shop_validate();
        if (shop_problems == 0) LOG_INFO("Shop system loaded");
        else                    LOG_WARN("Shop system loaded with %d content problem(s)",
                                         shop_problems);
    }

    if (zone_system_init(ZONES_PATH) >= 0) {
        LOG_INFO("Zone definitions loaded");
    } else {
        LOG_WARN("Zone definitions skipped (zones.json not found — "
                 "zone notifications disabled)");
    }

    if (!world_collision_init(WORLD_DAT_PATH)) {
        // Fatal on purpose. Without a collision map the server cannot tell open
        // ground from a wall, and starting anyway would mean running a world
        // where movement is unvalidated — which is worse than not running.
        LOG_ERROR("Cannot start without a collision map at '%s'. "
                  "Movement validation depends on it.", WORLD_DAT_PATH);
        return 1;
    }
    LOG_INFO("World collision map loaded");

    // Connect to database
    LOG_INFO("Connecting to database for world '%s'", g_server.server_name);

    /* The world roster is read from worlds.conf, so a world that is not listed
     * there fails here with a clear message instead of at the first NULL
     * connection string. */
    {
        char worlds_path[1024];
        if (!world_table_default_path(worlds_path, sizeof(worlds_path)) ||
            !world_table_load(worlds_path)) {
            LOG_ERROR("FAILED - no world table. Set MMO_WORLDS_CONF, or run "
                      "`make setup` so worlds.conf is packaged beside the binary.");
            return 1;
        }
    }

    const WorldEntry* self = world_table_by_name(g_server.server_name);
    if (!self) {
        LOG_ERROR("FAILED - world '%s' is not listed in worlds.conf",
                  g_server.server_name);
        return 1;
    }

    /* This world's identifier, so a ticket the realm minted for another world
     * can be refused at admission. Row order in worlds.conf assigns it, which
     * is the same rule the realm uses when it hands the identifier out. */
    g_server.world_id = self->id;
    playerdata_set_world_id(g_server.world_id);
    LOG_INFO("World id: %u", g_server.world_id);

    /* The realm listener's port comes from the same table the realm dials, so
     * the two cannot disagree. A `realm_port` line in the world's own .conf
     * wins, for a deployment that has to move it on one host only. */
    if (g_server.realm_port == 0) g_server.realm_port = self->realm_port;
    if (g_server.realm_port == g_server.port) {
        LOG_ERROR("FAILED - the realm port and the client port are both %u",
                  (unsigned)g_server.port);
        return 1;
    }

    const char* pg_conn_str = self->conninfo;

    /* The database name only. The full connection string may carry a password
     * once a deployment sets one, and this line goes to a log everyone reads. */
    LOG_INFO("Database: %s", self->database);

    if (!playerdata_init(pg_conn_str)) {
        LOG_ERROR("FAILED - PostgreSQL initialization");
        return 1;
    }

    LOG_INFO("✓ Connected to world database");

    /* Seed the item-instance allocator from what is already persisted.
     *
     * item_instance_next_id() hands out process-local identifiers starting at 1.
     * Without this call every restart begins reissuing identifiers that live
     * character_items rows already own: the next save collides on
     * PK(instance_id) / UNIQUE(character_id, slot), and the rows that do land
     * overwrite another character's items. The defect is silent -- nothing fails
     * until two characters' inventories have already merged.
     *
     * A failed query is fatal rather than seeded as zero. The query returns 0
     * both for an empty table and for a failure, and guessing "empty" is exactly
     * the case that reissues live identifiers.
     */
    {
        uint64_t highest_instance_id = 0;
        if (!character_items_max_instance_id_checked(&highest_instance_id)) {
            LOG_ERROR("FAILED - could not read the highest persisted item-instance id. "
                      "Starting anyway would reissue identifiers that persisted items "
                      "already own and corrupt inventories.");
            playerdata_close();
            return 1;
        }
        item_instance_seed(highest_instance_id);
        LOG_INFO("✓ Item-instance allocator seeded above %llu",
                 (unsigned long long)highest_instance_id);
    }

    if (!npc_world_init(&g_npc_world, g_server.max_npcs)) {
        LOG_ERROR("FAILED - could not allocate the NPC pool");
        playerdata_close();
        return 1;
    }

    /* The index packet threads query instead of scanning the pool. Allocated
     * here, published every tick by the gameplay thread. */
    if (!npc_query_init(&g_npc_world)) {
        LOG_ERROR("FAILED - could not allocate the shared NPC query index");
        npc_world_shutdown(&g_npc_world);
        playerdata_close();
        return 1;
    }
    party_init();

    // Load NPC spawns from data file
    {
        int spawn_count = npc_spawns_load(SPAWNS_PATH, &g_npc_world);
        if (spawn_count < 0) {
            LOG_ERROR("FAILED - Could not load NPC spawns from %s", SPAWNS_PATH);
            return 1;
        }
        LOG_INFO("Loaded %d NPC spawns", spawn_count);
    }

    if (!playerdata_start_save_thread()) {
        LOG_ERROR("FAILED - Periodic save thread");
        playerdata_close();
        return 1;
    }

    g_server.tcp_sockfd = create_tcp_server_socket(g_server.port);
    if (g_server.tcp_sockfd < 0) {
        playerdata_close();
        return 1;
    }

    /* The realm link is mutual TLS, and the identity has to load before the
     * listener opens: a world that accepted realm connections while it had no
     * certificate would be accepting them in plaintext, which is the one
     * outcome worse than refusing them. */
    if (!realm_link_tls_init("./certs/server.crt", "./certs/server.key",
                             "./certs/realm_pins.txt")) {
        LOG_ERROR("FAILED - the realm link's TLS identity could not be loaded");
        close(g_server.tcp_sockfd);
        playerdata_close();
        return 1;
    }

    /* The realm link's own listener. Fatal if it cannot be opened: a world the
     * realm cannot reach is a world nobody can enter, and failing at startup
     * says so once instead of appearing later as an unexplained absence from
     * the world list. */
    g_server.realm_sockfd = create_bound_server_socket(g_server.realm_bind,
                                                       g_server.realm_port);
    if (g_server.realm_sockfd < 0) {
        LOG_ERROR("FAILED - could not open the realm listener on port %u",
                  (unsigned)g_server.realm_port);
        realm_link_tls_cleanup();
        close(g_server.tcp_sockfd);
        playerdata_close();
        return 1;
    }

    /* START CHAT DISPATCH — before the loops, so no packet can be routed to a
     * chat queue that does not exist yet. */
    if (!chat_init()) {
        LOG_ERROR("FAILED - could not start the chat dispatch thread");
        g_server.running = 0;
        playerdata_close();
        return 1;
    }

    // INIT EVENT LOOPS — connection count is no longer bounded by thread count.
    if (net_loop_start(net_tuning_workers("MMO_WORLD_WORKERS")) != 0) {
        LOG_ERROR("FAILED - could not start event loops");
        g_server.running = 0;
        playerdata_close();
        return 1;
    }

    // START ACCEPT THREAD
    if (pthread_create(&g_server.accept_thread, NULL, accept_thread_func, NULL) != 0) {
        LOG_ERROR("FAILED - Accept thread");
        close(g_server.tcp_sockfd);
        close(g_server.realm_sockfd);
        playerdata_close();
        return 1;
    }

    /* START THE METRICS ENDPOINT — after everything it reports exists, and
     * before the banner, so a failure to bind is reported next to the other
     * startup problems rather than after "server ready". Each world offsets
     * the configured base port by its own id, so one MMO_METRICS_PORT
     * configures the whole stack. */
    metrics_server_start(g_server.server_name,
                         metrics_bind_from_env(),
                         metrics_port_from_env((int)g_server.world_id),
                         world_metrics, world_health, NULL);

    // START REALM ACCEPT THREAD
    if (pthread_create(&g_server.realm_accept_thread, NULL,
                       realm_accept_thread_func, NULL) != 0) {
        LOG_ERROR("FAILED - Realm accept thread");
        g_server.running = 0;
        close(g_server.tcp_sockfd);
        close(g_server.realm_sockfd);
        pthread_join(g_server.accept_thread, NULL);
        playerdata_close();
        return 1;
    }

    // START COMBAT THREAD (20Hz)
    if (pthread_create(&g_combat_thread, NULL, combat_update_thread, NULL) != 0) {
        LOG_ERROR("FAILED - Combat update thread");
        g_server.running = 0;
        pthread_join(g_server.accept_thread, NULL);
        close(g_server.tcp_sockfd);
        playerdata_close();
        return 1;
    }

    // START WORLD BROADCAST THREAD (projectiles 30Hz, players 20Hz, npcs 10Hz)
    g_broadcast_running = 1;
    if (pthread_create(&g_broadcast_thread, NULL, world_broadcast_thread, NULL) != 0) {
        LOG_ERROR("FAILED - World broadcast thread");
        g_broadcast_running = 0;
        g_combat_running = 0;
        g_server.running = 0;
        pthread_join(g_combat_thread, NULL);
        pthread_join(g_server.accept_thread, NULL);
        close(g_server.tcp_sockfd);
        playerdata_close();
        return 1;
    }

    LOG_INFO("✓ All systems online - server ready");
    LOG_INFO("  - Accept thread: Running (clients on %u, realm on %u)",
             (unsigned)g_server.port, (unsigned)g_server.realm_port);
    LOG_INFO("  - Event loops: epoll (see [NET] line above for counts)");
    LOG_INFO("  - Combat thread: 20Hz");
    LOG_INFO("  - World broadcast thread: projectiles 30Hz, players 20Hz, npcs 10Hz");

    // MAIN THREAD: Wait for shutdown signal
    while (g_server.running) {
        sleep(1);

        // Print server stats every 10 seconds
        static time_t last_stats = 0;
        time_t now = time(NULL);
        if (now - last_stats >= 10) {
            /* Players and uptime used to be the whole line, which said nothing
             * about whether the world was keeping its tick, whether anything
             * was queueing behind it, or whether it was refusing traffic --
             * the three things someone reads a log for when a world is
             * misbehaving. /metrics carries all of it for a scraper; this is
             * the same numbers for a deployment that has not set one up. */
            unsigned long long lim_dropped = 0, lim_kicked = 0;
            packet_limiter_totals(NULL, &lim_dropped, &lim_kicked);

            int job_capacity = 0;
            int job_depth = net_loop_job_depth(&job_capacity);

            int pool_in_use = 0, pool_size = 0, pool_waiters = 0;
            character_database_pool_stats(&pool_in_use, &pool_size, &pool_waiters);

            LOG_INFO("[STATS] players=%d/%d uptime=%lds "
                     "tick=%.1f/%.1fms(mean/worst) "
                     "queues=chat:%zu,jobs:%d/%d db=%d/%d(+%d waiting) "
                     "limiter=%llu dropped,%llu kicked",
                     g_state.current_players, g_server.max_players,
                     (long)(now - g_server_start_time),
                     g_published_tick_mean_ms, g_published_tick_worst_ms,
                     chat_queue_depth(), job_depth, job_capacity,
                     pool_in_use, pool_size, pool_waiters,
                     lim_dropped, lim_kicked);
            last_stats = now;
        }
    }

    if (g_shutdown_signal) {
        LOG_INFO("[SIGNAL] Received signal %d, shutting down gracefully...",
                 (int)g_shutdown_signal);
    }
    LOG_INFO("Shutting down...");

    /* Tell the players before the sockets go quiet.
     *
     * protocol.h has defined DISCONNECT_REASON_SHUTDOWN since the first
     * version and nothing ever sent it: the world simply stopped answering,
     * and every client sat there until its own ping timeout expired ~35s
     * later and reported a lost connection. One packet turns that into "the
     * server is restarting", which is the difference between a reconnect
     * prompt and a bug report.
     *
     * Sent here rather than from net_loop_stop(): the loops must still be
     * running for connection_io_send() to flush, and the accept thread is
     * already closed above so no new connection can arrive behind it. */
    broadcast_shutdown_notice();

    // Stop the broadcast and combat threads
    g_broadcast_running = 0;
    g_combat_running = 0;

    pthread_join(g_broadcast_thread, NULL);
    pthread_join(g_combat_thread, NULL);

    metrics_server_stop();

    // Stop both accept threads
    close(g_server.tcp_sockfd);
    close(g_server.realm_sockfd);
    pthread_join(g_server.accept_thread, NULL);
    pthread_join(g_server.realm_accept_thread, NULL);

    // Drain and shut down the event loops and their blocking workers
    net_loop_stop();

    /* After the loops, so nothing can enqueue behind the drain; the dispatcher
     * delivers what is already queued before it exits. */
    chat_shutdown();

    /* After the realm accept thread is joined, so no handler is still using it. */
    realm_link_tls_cleanup();

    // Cleanup
    npc_ai_cleanup();
    npc_query_shutdown();
    npc_world_shutdown(&g_npc_world);
    loot_cleanup();
    projectile_cleanup();
    ability_handler_cleanup();
    abilities_cleanup();
    dialogue_system_cleanup();
    shop_session_shutdown();
    items_cleanup();
    playerdata_stop_save_thread();
    playerdata_close();
    world_collision_shutdown();
    zone_system_cleanup();
    connection_io_shutdown();
    session_registry_shutdown();
    session_close();

    LOG_INFO("World Server stopped");
    log_close();
    return 0;
}
