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
#include "metrics_server.h"

#include "players_database.h"
#include "character_connect.h"
#include "config.h"
#include "utils.h"
#include "routes.h"
#include "world_connect.h"
#include "realm_world_auth.h"
#include "world_database_manager.h"
#include "realm_net.h"
#include "net_tuning.h"
#include "class_stats.h"
#include "data_paths.h"
#include "world_table.h"
#include "tls.h"

#include <signal.h>
#include <errno.h>
#include <poll.h>
#include <arpa/inet.h>
#include <limits.h>
#include <time.h>
#include <stdatomic.h>
#include <string.h>

/** The realm's TLS context, serving the client link. Released at shutdown. */
static SSL_CTX* g_tls_ctx = NULL;

/** Seconds between heartbeat cycles, and the retry interval for an offline world. */
#define WORLD_QUERY_TIMEOUT 15

/** Default milliseconds allowed for each blocking phase of one world probe.
 *
 * Overridable with $MMO_WORLD_PROBE_TIMEOUT_MS. Three of these can elapse per
 * world -- connect, heartbeat wait, heartbeat read -- so the worst-case cost of
 * one probe is roughly three times this value, paid concurrently rather than
 * once per world in series.
 */
#define WORLD_PROBE_TIMEOUT_MS_DEFAULT 3000

/** Default number of worlds probed at the same time.
 *
 * Overridable with $MMO_WORLD_PROBE_WORKERS. The monitor used to probe every
 * world in series with a 5s poll and a 5s receive each, then sleep 10s: with
 * ten worlds that is up to ~110s per cycle, so a world's online flag and
 * population were stale by far more than a player waits at the world list.
 */
#define WORLD_PROBE_WORKERS_DEFAULT 8

/* recv_exact_timeout() lived here, reading world heartbeats off a bare
 * descriptor. The world link is TLS now and reads go through tls_recv_exact(),
 * which additionally checks SSL_pending() before polling -- one TLS record can
 * carry more than one protocol packet, and a poll-first loop would wait out the
 * whole timeout on bytes already sitting decrypted in the session. */

/** Read a positive integer from the environment, or return a default. */
static int env_int(const char* name, int fallback, int low, int high) {
    const char* raw = getenv(name);
    if (!raw || !*raw) return fallback;

    char* end = NULL;
    long parsed = strtol(raw, &end, 10);
    if (end == raw || *end || parsed < low || parsed > high) {
        LOG_ERROR("%s='%s' is out of range [%d, %d]; using %d",
                  name, raw, low, high, fallback);
        return fallback;
    }
    return (int)parsed;
}

/**
 * Authenticate one realm client and route its reassembled post-authentication packets.
 *
 * The function owns and frees the heap-allocated descriptor argument, then closes the client descriptor before returning.
 *
 * @return      Always NULL.
 */
/**
 * Accept realm connections and hand each one to the reactor.
 *
 * One thread, not one per client. What used to live here -- a poll loop, a
 * reassembly buffer, the session handshake and the packet drain, all repeated
 * per connection on its own detached thread -- is now realm_net.c on top of the
 * shared reactor, so the thread count no longer tracks the player count.
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

        LOG_INFO("[REALM] new connection from %s:%d",
                 inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));

        /* Takes ownership of the descriptor either way. */
        realm_net_submit(client_fd);
    }

    return NULL;
}

/** One world's probe, and the shared claim counter the workers pull from. */
typedef struct {
    WorldServer*  worlds;
    int           count;
    _Atomic int   next;        /**< Next unclaimed index; workers fetch-and-add. */
    const char*   fallback_key; /**< Used when a world has no key of its own. */
    time_t        now;
    int           timeout_ms;
} ProbeBatch;

/** Close one world link, session and descriptor together.
 *
 * They are acquired together and must be released together: closing the
 * descriptor alone would leave the session pointing at a number the kernel is
 * free to hand to the next connection, and the next heartbeat would encrypt
 * this realm's traffic into somebody else's socket.
 */
static void world_link_drop(WorldServer* ws) {
    if (ws->tls) { tls_close((SSL*)ws->tls); ws->tls = NULL; }
    if (ws->fd >= 0) { close(ws->fd); ws->fd = -1; }
}

/** Probe one world in place: connect when down, heartbeat when up.
 *
 * Operates on the caller's private snapshot, so no lock is held across any of
 * the network I/O here.
 */
static void probe_one_world(WorldServer* ws, const ProbeBatch* batch) {
    time_t now = batch->now;

    if (ws->fd < 0) {
        if (now - ws->last_heartbeat < WORLD_QUERY_TIMEOUT) return;

        char* world_key = get_server_auth_key_from_redis(ws->name);
        const char* key_to_use = world_key ? world_key : batch->fallback_key;

        // silent=1 suppresses the per-attempt spam; the first failure is logged below.
        SSL* session = NULL;
        ws->fd = connect_to_world_server(ws->host, ws->realm_port, ws->name,
                                         key_to_use, &session, 1,
                                         batch->timeout_ms);
        ws->tls = session;
        ws->last_heartbeat = now;

        free(world_key);

        if (ws->fd < 0) {
            if (!ws->connection_logged) {
                LOG_INFO("World server '%s' is offline - will retry silently every %ds",
                         ws->name, WORLD_QUERY_TIMEOUT);
                ws->connection_logged = 1;
            }
            ws->online = 0;
            ws->player_count = 0;  // Reset player count when offline
        } else {
            LOG_INFO("Successfully connected to world server '%s'", ws->name);
            ws->online = 1;
            ws->connection_logged = 0;  // Reset for next disconnect
        }
        return;
    }

    WorldHeartbeatPacket hb = {0};
    hb.header.type = PACKET_WORLD_HEARTBEAT;
    hb.header.player_id = 0;
    hb.header.payload_size = 0;
    hb.timestamp = now;

    if (!tls_send_exact(ws->tls, &hb, sizeof(hb), batch->timeout_ms)) {
        world_link_drop(ws);
        ws->online = 0;
        ws->last_heartbeat = now;
        return;
    }

    WorldStatusPacket status;
    if (!tls_recv_exact(ws->tls, &status, sizeof(status), batch->timeout_ms)) {
        world_link_drop(ws);
        ws->online = 0;
        ws->last_heartbeat = now;
        return;
    }

    if (status.header.type == PACKET_WORLD_STATUS) {
        ws->player_count = ntohl(status.player_count);
        ws->max_players = ntohl(status.max_players);
        ws->online = (status.status == 1);
        ws->last_heartbeat = now;

        LOG_DEBUG("[%s] Status: %u/%u players, uptime: %lu seconds",
                  ws->name, ws->player_count, ws->max_players,
                  (unsigned long)status.uptime);
    }
}

/** Claim world indices from the batch until none are left.
 *
 * Index claiming rather than a fixed slice per worker: worlds do not cost the
 * same to probe -- an online one answers in a millisecond, an unreachable one
 * costs the full deadline -- so a static split would leave workers idle while
 * one of them sat on every timeout in its slice.
 */
static void* probe_worker(void* arg) {
    ProbeBatch* batch = arg;
    for (;;) {
        int index = atomic_fetch_add(&batch->next, 1);
        if (index >= batch->count) return NULL;
        probe_one_world(&batch->worlds[index], batch);
    }
}

/** Probe every world concurrently, returning once all of them have answered. */
static void probe_all_worlds(ProbeBatch* batch, int worker_limit) {
    int workers = batch->count < worker_limit ? batch->count : worker_limit;
    if (workers < 1) workers = 1;

    pthread_t* threads = calloc((size_t)workers, sizeof(*threads));
    if (!threads) {
        /* Out of memory is not a reason to stop monitoring; fall back to
         * probing them one at a time on this thread. */
        probe_worker(batch);
        return;
    }

    int started = 0;
    for (int i = 0; i < workers; i++) {
        if (pthread_create(&threads[i], NULL, probe_worker, batch) == 0) started++;
        else break;
    }

    /* This thread is a worker too, so a batch still completes even when no
     * additional thread could be created. */
    probe_worker(batch);

    for (int i = 0; i < started; i++) pthread_join(threads[i], NULL);
    free(threads);
}

/**
 * Monitor every configured world, probing them concurrently once per cycle.
 *
 * @return Always NULL after the realm stops.
 */
void* world_monitor_thread_func(void* arg) {
    (void)arg;

    int timeout_ms = env_int("MMO_WORLD_PROBE_TIMEOUT_MS",
                             WORLD_PROBE_TIMEOUT_MS_DEFAULT, 100, 60000);
    int worker_limit = env_int("MMO_WORLD_PROBE_WORKERS",
                               WORLD_PROBE_WORKERS_DEFAULT, 1, 256);

    pthread_mutex_lock(&g_server.world_servers_lock);
    int world_count = g_server.num_world_servers;
    pthread_mutex_unlock(&g_server.world_servers_lock);

    if (world_count <= 0) {
        LOG_WARN("No world servers configured; the monitor has nothing to do");
        return NULL;
    }

    WorldServer* probe = calloc((size_t)world_count, sizeof(*probe));
    if (!probe) {
        LOG_ERROR("Out of memory allocating the world probe snapshot");
        return NULL;
    }

    LOG_INFO("World monitor thread started (%d worlds, up to %d probed at once, "
             "%dms per phase)", world_count,
             worker_limit < world_count ? worker_limit : world_count, timeout_ms);

    while (g_server.running) {
        time_t now = time(NULL);

        // Get today's server auth key from Redis
        char* server_key = get_server_auth_key_from_redis("global");
        if (!server_key) {
            LOG_WARN("WARNING: No server auth key in Redis, retrying in %ds",
                     WORLD_QUERY_TIMEOUT);
            sleep(WORLD_QUERY_TIMEOUT);
            continue;
        }

        // Probe every world against a private snapshot, with the lock RELEASED.
        //
        // Each probe performs blocking network I/O. Holding world_servers_lock
        // across it stalled every client that wanted to list worlds or enter
        // one, because those paths take the same lock -- so one unresponsive
        // world delayed everybody's login by up to its full timeout. Only this
        // thread and its workers ever mutate fd, and the shutdown sweep below
        // runs after this loop exits, so the snapshot cannot race a writer.
        pthread_mutex_lock(&g_server.world_servers_lock);
        memcpy(probe, g_server.world_servers,
               sizeof(WorldServer) * (size_t)world_count);
        pthread_mutex_unlock(&g_server.world_servers_lock);

        ProbeBatch batch = {
            .worlds       = probe,
            .count        = world_count,
            .fallback_key = server_key,
            .now          = now,
            .timeout_ms   = timeout_ms,
        };
        atomic_init(&batch.next, 0);

        probe_all_worlds(&batch, worker_limit);

        // Publish the probe results. Name, region, host and port come from
        // config and are never mutated here, so only live status is written back.
        pthread_mutex_lock(&g_server.world_servers_lock);
        for (int i = 0; i < world_count; i++) {
            WorldServer* dst = &g_server.world_servers[i];
            dst->fd                = probe[i].fd;
            dst->tls               = probe[i].tls;
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

    free(probe);

    // Cleanup connections
    pthread_mutex_lock(&g_server.world_servers_lock);
    for (int i = 0; i < g_server.num_world_servers; i++) {
        world_link_drop(&g_server.world_servers[i]);
    }
    pthread_mutex_unlock(&g_server.world_servers_lock);

    LOG_INFO("World monitor thread exiting");
    return NULL;
}

/**
 * Initialize realm dependencies and run client and world-monitor threads until shutdown.
 *
 * @param argc  Argument count; an optional first argument names the configuration file.
 * @param argv  Argument vector containing the optional configuration path.
 * @return      Zero after orderly shutdown, or one when initialization fails.
 */
/* --- The metrics endpoint ------------------------------------------------ */

/** When this process started, for the uptime metric. */
static time_t g_realm_start_time;

/** Write the realm's numbers in Prometheus text format. */
static size_t realm_metrics(char* out, size_t out_size, void* user) {
    (void)user;
    size_t used = 0;

    metrics_write(out, out_size, &used, "mmo_realm_connections",
                  "Open client connections", "gauge",
                  (double)realm_net_connection_count());
    metrics_write(out, out_size, &used, "mmo_realm_uptime_seconds",
                  "Seconds since this realm started", "counter",
                  (double)(time(NULL) - g_realm_start_time));

    /* The world roster, as the realm currently sees it. Worlds online versus
     * worlds configured is the one number that says whether the thing players
     * are about to be shown is the thing that exists. */
    int configured = 0, online = 0;
    uint32_t players = 0, capacity = 0;

    pthread_mutex_lock(&g_server.world_servers_lock);
    configured = g_server.num_world_servers;
    for (int i = 0; i < configured; i++) {
        if (!g_server.world_servers[i].online) continue;
        online++;
        players  += g_server.world_servers[i].player_count;
        capacity += g_server.world_servers[i].max_players;
    }
    pthread_mutex_unlock(&g_server.world_servers_lock);

    metrics_write(out, out_size, &used, "mmo_realm_worlds_configured",
                  "Worlds listed in worlds.conf", "gauge", (double)configured);
    metrics_write(out, out_size, &used, "mmo_realm_worlds_online",
                  "Worlds answering the realm heartbeat", "gauge", (double)online);
    metrics_write(out, out_size, &used, "mmo_realm_players_total",
                  "Players across every online world", "gauge", (double)players);
    metrics_write(out, out_size, &used, "mmo_realm_capacity_total",
                  "Capacity across every online world", "gauge", (double)capacity);

    /* What the packet limiter has refused on the character-select link. Its
     * only output was a rate-limited log line, so a realm being hammered
     * looked like a quiet one to anything scraping it. */
    unsigned long long lim_allowed = 0, lim_dropped = 0, lim_kicked = 0;
    packet_limiter_totals(&lim_allowed, &lim_dropped, &lim_kicked);
    metrics_write(out, out_size, &used, "mmo_realm_packets_allowed_total",
                  "Packets accepted by the rate limiter since startup", "counter",
                  (double)lim_allowed);
    metrics_write(out, out_size, &used, "mmo_realm_packets_dropped_total",
                  "Packets dropped for exceeding a packet budget since startup", "counter",
                  (double)lim_dropped);
    metrics_write(out, out_size, &used, "mmo_realm_limiter_kicks_total",
                  "Connections closed for sustained packet-budget abuse since startup",
                  "counter", (double)lim_kicked);

    return used;
}

/** Report whether the realm can still put a player into a world.
 *
 * It needs Redis to mint a ticket, and it needs at least one world to send
 * them to. A realm with every world offline is reachable, authenticating, and
 * of no use to anybody -- which is precisely the state worth alerting on and
 * precisely the state that used to be invisible.
 */
static int realm_health(char* reason, size_t reason_size, void* user) {
    (void)user;

    if (!g_server.running) {
        snprintf(reason, reason_size, "shutting down");
        return 0;
    }
    if (!session_is_ready()) {
        snprintf(reason, reason_size, "Redis is unavailable; no ticket can be issued");
        return 0;
    }

    int online = 0, configured = 0;
    pthread_mutex_lock(&g_server.world_servers_lock);
    configured = g_server.num_world_servers;
    for (int i = 0; i < configured; i++)
        if (g_server.world_servers[i].online) online++;
    pthread_mutex_unlock(&g_server.world_servers_lock);

    if (online == 0) {
        snprintf(reason, reason_size, "none of the %d configured worlds are online",
                 configured);
        return 0;
    }
    return 1;
}

int main(int argc, char** argv) {
    log_init();   // reads MMO_LOG_LEVEL; must run before any thread starts

    LOG_INFO("=== REALM SERVER ===");
    LOG_INFO("PID: %d", getpid());

    // Sizes its slot table from RLIMIT_NOFILE, so it must run after any
    // descriptor limit changes and before the accept loop starts.
    packet_limiter_init(limit_profile_realm());

    /* The world roster. One file -- worlds.conf -- is the source for the
     * realm's world list, each world's database, and every world's display
     * name. It used to be three hardcoded C ladders plus a separate
     * worlds.txt, six coordinated edits to add a world. */
    {
        char worlds_path[1024];
        if (!world_table_default_path(worlds_path, sizeof(worlds_path)) ||
            !world_table_load(worlds_path)) {
            LOG_ERROR("FATAL: no world table. Set MMO_WORLDS_CONF, or run "
                      "`make setup` so worlds.conf is packaged beside the binary.");
            return 1;
        }
        LOG_INFO("Loaded %zu worlds from %s", world_table_count(), worlds_path);
    }

    /* The realm server owns character creation, so it needs the race registry to
     * validate what a client asks to create. Without it every create is refused,
     * which is the correct failure for a missing registry but a confusing one, so
     * say plainly what happened. */
    char races_path[512], progression_path[512];
    data_path_resolve(races_path, sizeof(races_path), "/data/races.json");
    data_path_resolve(progression_path, sizeof(progression_path), "/data/progression.json");
    if (!class_stats_init(races_path, progression_path)) {
        LOG_ERROR("Failed to load the race registry from %s — "
                          "character creation will refuse every request", races_path);
    }

    if (!session_init()) {
        LOG_ERROR("Failed to initialize Redis session connection");
        return 1;
    }

    /* The server-to-server auth key must already be provisioned.
     *
     * This used to write "default_server_secret_123" into Redis with no expiry
     * whenever the key was missing, which meant the credential guarding every
     * realm-to-world handshake was a string in this repository -- and, because
     * it was written without a TTL, it outlived every rotation that followed.
     * A missing key is an operational failure, not something to paper over with
     * a known value.
     */
    {
        char* key = get_server_auth_key_from_redis("global");
        if (!key) {
            LOG_ERROR("FATAL: no server auth key at Redis key 'server_auth_key:global'.\n"
                      "       Provision one before starting the realm, e.g.\n"
                      "         common/server_keys/generate_daily_server_keys.sh\n"
                      "       The realm cannot authenticate to any world without it.");
            session_close();
            return 1;
        }
        /* Never logged: it is the credential itself. */
        free(key);
    }

    memset(&g_server, 0, sizeof(g_server));
    memset(&g_state, 0, sizeof(g_state));
    g_server.running = 1;  // Initialize running state
    g_realm_start_time = time(NULL);
    pthread_mutex_init(&g_server.world_servers_lock, NULL);

    /* Allocated once, sized from the roster, before any thread can read it.
     * The monitor thread used to do this load itself, which meant the world
     * list was empty for however long the first probe cycle took. */
    {
        size_t configured = world_table_count();
        g_server.world_servers = calloc(configured, sizeof(*g_server.world_servers));
        if (!g_server.world_servers) {
            LOG_ERROR("Out of memory allocating %zu world slots", configured);
            session_close();
            return 1;
        }
        g_server.num_world_servers =
            load_world_servers_from_table(g_server.world_servers, (int)configured);
        if (g_server.num_world_servers <= 0) {
            LOG_ERROR("FATAL: the world table produced no usable worlds");
            free(g_server.world_servers);
            session_close();
            return 1;
        }
    }

    // Load config if provided
    if (argc > 1) {
        if (!set_config(argv[1])) {
            LOG_INFO("Usage: ./realm_server <config_file.conf>");
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

    /* The client link is TLS. The realm answers character lists, creations and
     * deletions, and every one of those connections opens by repeating the
     * session key the login server minted -- so this hop carries as much as the
     * login hop does and is encrypted on the same terms.
     *
     * Startup fails without a certificate rather than falling back to
     * plaintext. A silent downgrade is the worst of both: the operator believes
     * the link is encrypted, and nothing in a running system says otherwise. */
    g_tls_ctx = tls_server_init("./certs/server.crt", "./certs/server.key");
    if (!g_tls_ctx) {
        LOG_ERROR("Failed to initialize the realm TLS context.");
        LOG_INFO("Run setup/setup.sh, which generates this certificate and");
        LOG_INFO("installs the matching pin into the client tree. By hand:");
        LOG_INFO("  mkdir -p certs && openssl req -x509 -newkey rsa:2048 \\");
        LOG_INFO("    -keyout certs/server.key -out certs/server.crt \\");
        LOG_INFO("    -days 3650 -nodes -subj \"/CN=mmo-realm\"");
        close(g_server.tcp_sockfd);
        character_database_close();
        session_close();
        return 1;
    }

    /* The world link is mutual TLS and uses the same certificate. One identity
     * per service: this process is "the realm" to a client and to a world
     * alike, and giving it two would mean two things to rotate and two chances
     * to rotate only one. */
    if (!world_connect_tls_init("./certs/server.crt", "./certs/server.key",
                                "./certs/world_pins.txt")) {
        LOG_ERROR("Realm↔world TLS could not be initialized.");
        LOG_INFO("Run setup/setup.sh, which generates every certificate in this");
        LOG_INFO("system and cross-installs the pins.");
        tls_server_cleanup(g_tls_ctx);
        close(g_server.tcp_sockfd);
        character_database_close();
        session_close();
        return 1;
    }

    if (realm_net_start(g_tls_ctx, net_tuning_workers("MMO_REALM_WORKERS")) != 0) {
        LOG_ERROR("Failed to start the realm event loops");
        tls_server_cleanup(g_tls_ctx);
        close(g_server.tcp_sockfd);
        character_database_close();
        session_close();
        return 1;
    }

    if (pthread_create(&g_server.accept_thread, NULL, accept_thread_func, NULL) != 0) {
        realm_net_stop();
        close(g_server.tcp_sockfd);
        character_database_close();
        session_close();
        return 1;
    }

    // Start world server monitoring thread
    if (pthread_create(&g_server.world_monitor_thread, NULL, world_monitor_thread_func, NULL) != 0) {
        LOG_ERROR("Failed to create world monitor thread");
        g_server.running = 0;
        pthread_join(g_server.accept_thread, NULL);
        realm_net_stop();
        close(g_server.tcp_sockfd);
        character_database_close();
        session_close();
        return 1;
    }

    /* Offset 1: the login server takes the configured base port and the realm
     * the one after it, so each world's own id lands clear of both. */
    metrics_server_start("realm", metrics_bind_from_env(),
                         metrics_port_from_env(1),
                         realm_metrics, realm_health, NULL);

    LOG_INFO("Realm Server '%s' running on port %d", g_server.name, g_server.port);

    while (g_server.running) {
        sleep(1);
    }

    LOG_INFO("\nShutting down...");
    metrics_server_stop();
    close(g_server.tcp_sockfd);
    pthread_join(g_server.accept_thread, NULL);
    pthread_join(g_server.world_monitor_thread, NULL);
    realm_net_stop();
    world_connect_tls_cleanup();
    tls_server_cleanup(g_tls_ctx);
    g_tls_ctx = NULL;
    world_databases_cleanup();
    session_close();
    pthread_mutex_destroy(&g_server.world_servers_lock);
    free(g_server.world_servers);
    g_server.world_servers = NULL;
    world_table_free();

    LOG_INFO("Realm Server stopped");
    log_close();
    return 0;
}
