/**
 * @file
 * Start the TLS login service and accept one authentication request per connection.
 *
 * The accept loop lives here; everything a connection does after being accepted
 * lives in login_net.c on top of the shared reactor.
 */
#include "types.h"
#include "metrics_server.h"
#include "log.h"
#include "session.h"
#include "auth.h"
#include "rate_limiter.h"
#include "users_database.h"
#include "config.h"
#include "routes.h"
#include "patch_notes.h"
#include "tls.h"
#include "login_net.h"
#include "net_tuning.h"

#include <stdlib.h>
#include <errno.h>
#include <poll.h>
#include <netinet/tcp.h>
#include <unistd.h>
#include <libgen.h>
#include <time.h>

ServerConfig g_server;
SSL_CTX* g_tls_ctx = NULL;

/** When this process started, for the uptime metric. */
static time_t g_login_start_time;

/**
 * Accept rate-limited login connections and hand each one to the reactor.
 *
 * The per-IP checks stay here, ahead of the reactor, so a blocked or
 * too-frequent address costs one accept and one close rather than a connection
 * slot and a TLS session.
 *
 * @return      Always NULL after the server stops or the accept loop fails.
 */
void* accept_thread_func(void* arg) {
    (void)arg;
    
    LOG_INFO("Accept thread started");
    
    struct pollfd pfd;
    pfd.fd = g_server.tcp_sockfd;
    pfd.events = POLLIN;
    
    while (g_server.running) {
        int ret = poll(&pfd, 1, 1000);
        
        if (ret < 0) {
            if (errno == EINTR) continue;
            LOG_INFO("poll error: %s", strerror(errno));
            break;
        }
        
        if (ret == 0) continue;
        if (!g_server.running) break;
        
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        
        int client_fd = accept(g_server.tcp_sockfd, (struct sockaddr*)&client_addr, &addr_len);
        
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            if (errno == EBADF) break;
            LOG_INFO("accept failed: %s", strerror(errno));
            continue;
        }
        
        // Resolved from the socket rather than formatted here: inet_ntoa
        // returns a shared static buffer and only understands IPv4.
        char peer_ip[RL_IP_MAXLEN] = {0};
        rate_limiter_peer_ip(client_fd, peer_ip, sizeof(peer_ip));
        LOG_INFO("New login from %s:%d (fd: %d)",
                 peer_ip[0] ? peer_ip : "?", ntohs(client_addr.sin_port), client_fd);

        if (rate_limiter_check(peer_ip)) {
            LOG_INFO("[RATE_LIMIT] Rejected blocked IP %s", peer_ip);
            close(client_fd);
            continue;
        }

        // Bound connection churn. Each connection costs a TLS handshake and a
        // thread, both of which are spent before any packet is even read.
        if (rate_limiter_record_connection(peer_ip)) {
            LOG_INFO("[RATE_LIMIT] Refused connection from %s (rate)", peer_ip);
            close(client_fd);
            continue;
        }

        /* Takes ownership of the descriptor either way. The handshake starts
         * on an event loop rather than here, so this thread is never the one
         * waiting on a client's TLS negotiation. */
        login_net_submit(client_fd);
    }
    
    LOG_INFO("Accept thread exiting");
    return NULL;
}

/**
 * Initialize login-server dependencies and run until a shutdown signal arrives.
 *
 * @param argc  Argument count; an optional first argument overrides the listening port.
 * @param argv  Argument vector containing the optional listening port.
 * @return      Zero after orderly shutdown, or one when initialization fails.
 */
/* --- The metrics endpoint ------------------------------------------------ */

/** Write the login server's numbers in Prometheus text format. */
static size_t login_metrics(char* out, size_t out_size, void* user) {
    (void)user;
    size_t used = 0;

    metrics_write(out, out_size, &used, "mmo_login_connections",
                  "Open client connections", "gauge",
                  (double)login_net_connection_count());
    metrics_write(out, out_size, &used, "mmo_login_uptime_seconds",
                  "Seconds since this login server started", "counter",
                  (double)(time(NULL) - g_login_start_time));

    /* The rate limiter's table size is the closest thing this service has to a
     * measure of how many distinct sources are talking to it, which is what a
     * distributed login flood looks like before anything else notices. */
    metrics_write(out, out_size, &used, "mmo_login_limiter_tracked",
                  "Peer address buckets the rate limiter is tracking", "gauge",
                  (double)rate_limiter_tracked());
    metrics_write(out, out_size, &used, "mmo_login_limiter_capacity",
                  "Slots the rate limiter table has grown to", "gauge",
                  (double)rate_limiter_capacity());

    return used;
}

/** Report whether the login server can still authenticate anybody.
 *
 * Redis is the whole answer: without it there is no session to create, so a
 * login server that cannot reach it is accepting connections it can only
 * refuse. That is exactly the state a load balancer should route around.
 */
static int login_health(char* reason, size_t reason_size, void* user) {
    (void)user;

    if (!g_server.running) {
        snprintf(reason, reason_size, "shutting down");
        return 0;
    }
    if (!session_is_ready()) {
        snprintf(reason, reason_size, "Redis is unavailable; no session can be created");
        return 0;
    }
    return 1;
}

int main(int argc, char** argv) {
    // chdir to the directory containing the binary so all relative paths work
    // regardless of where the server is launched from
    {
        char exe_path[4096];
        ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
        if (len > 0) {
            exe_path[len] = '\0';
            // A failure here is not fatal but is worth saying out loud: every
            // relative path below (certs/, databases/) would then resolve
            // against the caller's directory, and the first symptom would be a
            // confusing "cannot load certificate" rather than "wrong cwd".
            if (chdir(dirname(exe_path)) != 0 || chdir("..") != 0)  // bin/ -> login_server/
                LOG_ERROR("Warning: could not chdir to the install root: %s",
                          strerror(errno));
        }
    }

    log_init();   // reads MMO_LOG_LEVEL; must run before any thread starts

    LOG_INFO("=== LOGIN SERVER (Two-Stage Auth) ===");
    LOG_INFO("PID: %d", getpid());

    rate_limiter_init();
    patch_notes_init();

    // Initialize TLS — cert/key relative to server working directory
    g_tls_ctx = tls_server_init("./certs/server.crt", "./certs/server.key");
    if (!g_tls_ctx) {
        LOG_ERROR("Failed to initialize TLS context.");
        LOG_INFO("Generate a self-signed cert with:");
        LOG_INFO("  mkdir -p certs && openssl req -x509 -newkey rsa:2048 \\");
        LOG_INFO("    -keyout certs/server.key -out certs/server.crt \\");
        LOG_INFO("    -days 3650 -nodes -subj \"/CN=mmo-login\"");
        return 1;
    }

    // Initialize database
    if (!db_init(USERS_DB)) {
        LOG_ERROR("Failed to initialize database");
        return 1;
    }
    
    if (!session_init()) {
        LOG_ERROR("Failed to initialize session system");
        LOG_INFO("Make sure Redis is running: redis-cli PING");
        db_close();
        return 1;
    }
    LOG_INFO("✓ Session system initialized");
    LOG_INFO("✓ Two-stage authentication enabled");
    LOG_INFO("  - PACKET_AUTH_LOGIN (3) = Validate credentials");
    LOG_INFO("  - PACKET_START_GAME_REQUEST (6) = Create session");
    
    memset(&g_server, 0, sizeof(g_server));
    g_server.port = LOGIN_SERVER_PORT;
    g_server.running = 1;
    g_login_start_time = time(NULL);
    
    if (argc > 1) {
        g_server.port = atoi(argv[1]); 
        if (g_server.port <= 0) {
            LOG_ERROR("Invalid port: %s", argv[1]);
            db_close();
            return 1;
        }
    }
    
    setup_signals();
    
    g_server.tcp_sockfd = create_tcp_server_socket(g_server.port);
    if (g_server.tcp_sockfd < 0) {
        LOG_ERROR("Failed to create server socket");
        db_close();
        return 1;
    }
    
    if (login_net_start(g_tls_ctx, net_tuning_workers("MMO_LOGIN_WORKERS")) != 0) {
        LOG_ERROR("Failed to start the login event loops");
        close(g_server.tcp_sockfd);
        db_close();
        return 1;
    }

    if (pthread_create(&g_server.accept_thread, NULL, accept_thread_func, NULL) != 0) {
        LOG_ERROR("Failed to create accept thread");
        login_net_stop();
        close(g_server.tcp_sockfd);
        db_close();
        return 1;
    }
    
    /* The metrics endpoint takes the configured base port unchanged: the login
     * server is the first service in the chain, so offset 0 is its own. */
    metrics_server_start("login", metrics_bind_from_env(),
                         metrics_port_from_env(0),
                         login_metrics, login_health, NULL);

    LOG_INFO("Login Server running on port %d. Press Ctrl+C to stop.", g_server.port);
    
    while (g_server.running) {
        sleep(1);
    }
    
    LOG_INFO("\nShutting down...");
    metrics_server_stop();
    close(g_server.tcp_sockfd);
    pthread_join(g_server.accept_thread, NULL);
    login_net_stop();

    session_close();
    db_close();
    tls_server_cleanup(g_tls_ctx);
    LOG_INFO("Login Server stopped");

    log_close();
    return 0;
}
