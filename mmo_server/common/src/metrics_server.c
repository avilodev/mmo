/**
 * @file
 * The /healthz and /metrics endpoint.
 *
 * One thread, one connection at a time. That is a deliberate ceiling rather
 * than a limitation to be lifted later: a scrape happens every fifteen seconds
 * from one or two collectors, and giving this a thread pool would mean giving
 * an unauthenticated listener the ability to consume threads. Everything is
 * bounded -- the request is read into a fixed buffer with a deadline, the
 * response is built into a fixed buffer, and a client that stops reading is
 * dropped rather than waited on.
 */

#include "metrics_server.h"
#include "log.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/** Longest request line and headers accepted. Anything more is not a scrape. */
#define METRICS_REQUEST_MAX 2048

/** Largest response body a provider may produce. */
#define METRICS_BODY_MAX 16384

/** Milliseconds a single client may take over its request or its read. */
#define METRICS_CLIENT_TIMEOUT_MS 2000

/** Default port base when $MMO_METRICS_PORT is unset: disabled. */
#define METRICS_PORT_DISABLED 0

static struct {
    int              listen_fd;
    pthread_t        thread;
    _Atomic int      running;
    int              started;
    char             service[64];
    MetricsProvider  metrics;
    HealthProvider   health;
    void*            user;
} g_metrics = { .listen_fd = -1 };

void metrics_write(char* out, size_t size, size_t* used,
                   const char* name, const char* help, const char* type,
                   double value) {
    if (!out || !used || *used >= size) return;

    char block[512];
    int n = snprintf(block, sizeof(block),
                     "# HELP %s %s\n# TYPE %s %s\n%s %.6g\n",
                     name, help, name, type, name, value);
    if (n <= 0 || (size_t)n >= sizeof(block)) return;

    /* All or nothing. A metric cut in half by a full buffer is a parse error
     * for the collector, which is worse than a metric that is simply absent. */
    if (*used + (size_t)n >= size) return;

    memcpy(out + *used, block, (size_t)n);
    *used += (size_t)n;
    out[*used] = '\0';
}

int metrics_port_from_env(int offset) {
    const char* raw = getenv("MMO_METRICS_PORT");
    if (!raw || !*raw) return METRICS_PORT_DISABLED;

    int base = atoi(raw);
    if (base <= 0) return METRICS_PORT_DISABLED;

    int port = base + offset;
    if (port < 1 || port > 65535) {
        LOG_ERROR("[METRICS] base port %d plus offset %d is out of range; "
                  "the endpoint is disabled", base, offset);
        return METRICS_PORT_DISABLED;
    }
    return port;
}

const char* metrics_bind_from_env(void) {
    const char* bind_addr = getenv("MMO_METRICS_BIND");
    /* Loopback unless told otherwise. This listener answers without
     * authenticating anybody, and what it reports -- population, tick health,
     * pool saturation -- is exactly what an attacker would want to know about
     * when to strike. Scrape it from the same host, or from a private
     * interface named explicitly. */
    return (bind_addr && *bind_addr) ? bind_addr : "127.0.0.1";
}

/** Write a complete response and close, ignoring a peer that stopped reading. */
static void send_response(int fd, const char* status, const char* content_type,
                          const char* body, size_t body_len) {
    char head[256];
    int n = snprintf(head, sizeof(head),
                     "HTTP/1.1 %s\r\n"
                     "Content-Type: %s\r\n"
                     "Content-Length: %zu\r\n"
                     "Connection: close\r\n"
                     "\r\n",
                     status, content_type, body_len);
    if (n <= 0) return;

    /* MSG_NOSIGNAL: a collector that gives up mid-response must not take the
     * process with it. Partial writes are not retried -- the response is small
     * and the client is about to be closed either way. */
    if (send(fd, head, (size_t)n, MSG_NOSIGNAL) < 0) return;
    if (body_len) (void)send(fd, body, body_len, MSG_NOSIGNAL);
}

/** Read a request line with a deadline. @return 1 when one was read. */
static int read_request(int fd, char* out, size_t out_size) {
    size_t used = 0;

    while (used + 1 < out_size) {
        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        int ready = poll(&pfd, 1, METRICS_CLIENT_TIMEOUT_MS);
        if (ready <= 0) return 0;

        ssize_t got = recv(fd, out + used, out_size - used - 1, 0);
        if (got <= 0) return 0;
        used += (size_t)got;
        out[used] = '\0';

        /* The request line alone is enough to route on, and waiting for the
         * blank line after the headers would let a client hold the single
         * serving thread for the full timeout by sending headers forever. */
        if (strchr(out, '\n')) return 1;
    }
    return 0;   /* longer than any real scrape request */
}

/** Serve one connection, then close it. */
static void serve(int fd) {
    char request[METRICS_REQUEST_MAX];
    if (!read_request(fd, request, sizeof(request))) return;

    if (strncmp(request, "GET ", 4) != 0) {
        send_response(fd, "405 Method Not Allowed", "text/plain", "", 0);
        return;
    }

    const char* path = request + 4;

    if (strncmp(path, "/healthz", 8) == 0) {
        char reason[256] = {0};
        int healthy = 1;
        if (g_metrics.health)
            healthy = g_metrics.health(reason, sizeof(reason), g_metrics.user);

        if (healthy) {
            send_response(fd, "200 OK", "text/plain", "ok\n", 3);
        } else {
            char body[320];
            int n = snprintf(body, sizeof(body), "unhealthy: %s\n",
                             reason[0] ? reason : "unspecified");
            send_response(fd, "503 Service Unavailable", "text/plain",
                          body, n > 0 ? (size_t)n : 0);
        }
        return;
    }

    if (strncmp(path, "/metrics", 8) == 0) {
        char* body = malloc(METRICS_BODY_MAX);
        if (!body) {
            send_response(fd, "503 Service Unavailable", "text/plain", "", 0);
            return;
        }
        body[0] = '\0';
        size_t len = g_metrics.metrics
            ? g_metrics.metrics(body, METRICS_BODY_MAX, g_metrics.user)
            : 0;
        send_response(fd, "200 OK", "text/plain; version=0.0.4", body, len);
        free(body);
        return;
    }

    send_response(fd, "404 Not Found", "text/plain", "", 0);
}

static void* metrics_thread_main(void* arg) {
    (void)arg;

    while (atomic_load(&g_metrics.running)) {
        struct pollfd pfd = { .fd = g_metrics.listen_fd, .events = POLLIN };
        /* A one-second wait rather than a blocking accept, so shutdown is a
         * flag rather than a close racing an accept in another thread. */
        if (poll(&pfd, 1, 1000) <= 0) continue;

        int client = accept(g_metrics.listen_fd, NULL, NULL);
        if (client < 0) continue;

        serve(client);
        close(client);
    }

    return NULL;
}

int metrics_server_start(const char* service_name,
                         const char* bind_addr, int port,
                         MetricsProvider metrics, HealthProvider health,
                         void* user) {
    if (port <= 0) {
        LOG_INFO("[METRICS] endpoint disabled (set MMO_METRICS_PORT to enable)");
        return 0;
    }
    if (!metrics) {
        LOG_ERROR("[METRICS] refusing to start without a metrics provider");
        return 0;
    }
    if (g_metrics.started) {
        LOG_ERROR("[METRICS] endpoint already running");
        return 0;
    }

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        LOG_ERROR("[METRICS] socket failed: %s", strerror(errno));
        return 0;
    }

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)port);

    const char* where = (bind_addr && *bind_addr) ? bind_addr : "127.0.0.1";
    if (inet_pton(AF_INET, where, &addr.sin_addr) != 1) {
        LOG_ERROR("[METRICS] '%s' is not an address to bind to", where);
        close(fd);
        return 0;
    }

    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0 ||
        listen(fd, 8) < 0) {
        /* Not fatal to the service. A world that cannot expose metrics is
         * still a world that can carry players, and refusing to start over an
         * observability port would turn a monitoring problem into an outage. */
        LOG_ERROR("[METRICS] could not listen on %s:%d: %s — continuing without "
                  "the endpoint", where, port, strerror(errno));
        close(fd);
        return 0;
    }

    g_metrics.listen_fd = fd;
    g_metrics.metrics   = metrics;
    g_metrics.health    = health;
    g_metrics.user      = user;
    snprintf(g_metrics.service, sizeof(g_metrics.service), "%s",
             service_name ? service_name : "mmo");
    atomic_store(&g_metrics.running, 1);

    if (pthread_create(&g_metrics.thread, NULL, metrics_thread_main, NULL) != 0) {
        LOG_ERROR("[METRICS] could not start the endpoint thread");
        atomic_store(&g_metrics.running, 0);
        close(fd);
        g_metrics.listen_fd = -1;
        return 0;
    }

    g_metrics.started = 1;
    LOG_INFO("[METRICS] %s serving /healthz and /metrics on %s:%d",
             g_metrics.service, where, port);
    return 1;
}

void metrics_server_stop(void) {
    if (!g_metrics.started) return;

    atomic_store(&g_metrics.running, 0);
    pthread_join(g_metrics.thread, NULL);

    close(g_metrics.listen_fd);
    g_metrics.listen_fd = -1;
    g_metrics.started   = 0;

    LOG_INFO("[METRICS] endpoint stopped");
}
