/**
 * @file
 * Teach the generic reactor what a login connection is.
 *
 * A login connection is short: a TLS handshake, one packet, one answer, and it
 * is gone. What makes it worth putting on an event loop anyway is the shape of
 * the arrival pattern -- everybody at once, on the hour a patch drops -- and the
 * fact that the handshake is spent before the protocol is even reached.
 *
 * Two threads touch a connection here, in order and never at once. The loop
 * drives the handshake and reassembles the request; a worker answers it, because
 * answering means a password hash and a database round trip.
 */

#include "login_net.h"

#include "log.h"
#include "net_reactor.h"
#include "routes.h"
#include "tls.h"
#include "types.h"

#include <arpa/inet.h>
#include <string.h>

/** Blocking pool depth: how many credential checks may run at once.
 *
 * Not derived from the character database pool -- the login server never
 * touches it -- and not sized from cores either, which was the guess this
 * replaces. A credential check is an Argon2id verification, and Argon2id is
 * memory-hard on purpose: libsodium's INTERACTIVE preset touches 64 MiB per
 * verification. What limits it is therefore memory bandwidth, and that
 * saturates long before the cores do.
 *
 * Measured on this machine, 32 concurrent clients against the login server:
 *
 *     workers   throughput      p50        p99
 *        2         22/s      1403 ms    2338 ms
 *        4         24/s      1310 ms    2300 ms
 *        8         24/s      1156 ms    2598 ms
 *       16         23/s      1078 ms    3697 ms
 *       32         23/s       929 ms    3672 ms
 *
 * Throughput is flat from four onwards, so every worker past that buys nothing
 * and costs two things: a longer tail, because more requests are in flight
 * against the same fixed capacity, and up to 64 MiB of transient memory each --
 * sixteen workers is a gigabyte of Argon2id state during a login surge, which
 * is exactly when the machine can least afford it.
 *
 * Override with MMO_LOGIN_WORKERS after measuring on the real hardware; this
 * number is a property of that machine, not of this code. */
#define LOGIN_WORKERS       4

/** Reassembly capacity, and therefore the largest request this server accepts.
 *
 * A login request is a fixed, small packet; this is generous for one. It is
 * deliberately checked against directly rather than against MAX_PACKET_SIZE,
 * which is larger: a request between the two would pass a MAX_PACKET_SIZE check
 * and then die as "filled its buffer", which describes the symptom rather than
 * the cause. */
#define LOGIN_BUFFER_BYTES  4096
/** Silence allowed before a connection has asked for anything.
 *
 * The same 30 seconds the blocking receive timeout used to allow, now covering
 * the handshake and the request together rather than each read separately. */
#define LOGIN_TIMEOUT_SECS  30

/** Hold one login connection's TLS session. */
typedef struct {
    SSL* ssl;
} LoginConn;

static SSL_CTX*   g_ctx = NULL;
static NetReactor* g_reactor = NULL;

static LoginConn* login_conn(NetReactorConn* conn) {
    return (LoginConn*)net_reactor_conn_user(conn);
}

/* --- The TLS transport --------------------------------------------------- */

static int login_handshake(NetReactorConn* conn) {
    return tls_handshake_continue(login_conn(conn)->ssl);
}

static ssize_t login_recv(NetReactorConn* conn, void* buf, size_t len) {
    return tls_read(login_conn(conn)->ssl, buf, len);
}

/** Release the session. Tolerates a connection that never got one. */
static void login_transport_close(NetReactorConn* conn) {
    LoginConn* lc = login_conn(conn);
    if (!lc || !lc->ssl) return;
    tls_close(lc->ssl);
    lc->ssl = NULL;
}

static const NetReactorTransport LOGIN_TLS = {
    .handshake = login_handshake,
    .recv      = login_recv,
    .close     = login_transport_close,
};

/* --- Accepting ----------------------------------------------------------- */

/** Attach a TLS session, without waiting for the handshake to finish. */
static int login_on_accept(NetReactorConn* conn) {
    LoginConn* lc = login_conn(conn);
    int fd = net_reactor_conn_fd(conn);

    lc->ssl = tls_handshake_begin(g_ctx, fd);
    if (!lc->ssl) {
        LOG_ERROR("[LOGIN] could not start a TLS session for fd %d", fd);
        return -1;
    }
    return 0;
}

/* --- Reading ------------------------------------------------------------- */

/**
 * Wait for one whole request, then hand it to a worker.
 *
 * Nothing is consumed: the worker is given the bytes where they lie.
 */
static NetReactorVerdict login_on_data(NetReactorConn* conn, uint8_t* data,
                                       size_t len, size_t* consumed) {
    *consumed = 0;

    if (len < MIN_HEADER_SIZE) return NET_REACTOR_KEEP;

    const PacketHeader* header = (const PacketHeader*)data;
    size_t expected = MIN_HEADER_SIZE + ntohs(header->payload_size);

    if (expected > LOGIN_BUFFER_BYTES) {
        LOG_WARN("[LOGIN] fd %d announced a %zu byte request; this server accepts "
                 "at most %d — disconnecting",
                 net_reactor_conn_fd(conn), expected, LOGIN_BUFFER_BYTES);
        return NET_REACTOR_CLOSE;
    }

    if (len < expected) return NET_REACTOR_KEEP;
    return NET_REACTOR_TO_WORKER;
}

/**
 * Answer one request.
 *
 * Always closes afterwards: the login server handles exactly one packet per
 * connection, and always has.
 */
static NetReactorVerdict login_on_work(NetReactorConn* conn) {
    LoginConn* lc = login_conn(conn);
    int fd = net_reactor_conn_fd(conn);

    size_t len = 0;
    uint8_t* buf = net_reactor_conn_buffer(conn, &len);
    if (len < MIN_HEADER_SIZE) return NET_REACTOR_CLOSE;

    const PacketHeader* header = (const PacketHeader*)buf;
    size_t expected = MIN_HEADER_SIZE + ntohs(header->payload_size);
    if (len < expected) return NET_REACTOR_CLOSE;

    LOG_DEBUG("[LOGIN] fd %d sent packet type %d (%zu bytes)",
              fd, header->type, expected);

    /* The reply path reaches its session through a thread-local, so bind it to
     * whichever worker picked this up -- and unbind it again, because the same
     * worker will go on to serve other connections. */
    tls_set_conn(lc->ssl);
    route_packet(fd, buf, (ssize_t)expected);
    tls_set_conn(NULL);

    return NET_REACTOR_CLOSE;
}

/* --- Leaving ------------------------------------------------------------- */

static int login_on_idle(NetReactorConn* conn, long idle_seconds) {
    if (idle_seconds <= LOGIN_TIMEOUT_SECS) return 0;
    LOG_INFO("[TIMEOUT] fd=%d opened a login connection and never asked for "
             "anything (%lds) — dropping", net_reactor_conn_fd(conn), idle_seconds);
    return 1;
}

/* --- Lifecycle ----------------------------------------------------------- */

int login_net_start(SSL_CTX* ctx, int worker_count) {
    if (!ctx) return -1;
    g_ctx = ctx;

    NetReactorConfig config;
    memset(&config, 0, sizeof(config));

    config.name         = "LOGIN";
    config.worker_count = worker_count > 0 ? worker_count : LOGIN_WORKERS;
    config.buffer_size  = LOGIN_BUFFER_BYTES;
    config.user_size    = sizeof(LoginConn);
    config.transport    = &LOGIN_TLS;

    config.on_accept    = login_on_accept;
    config.on_data      = login_on_data;
    config.on_work      = login_on_work;
    config.on_idle      = login_on_idle;

    g_reactor = net_reactor_start(&config);
    return g_reactor ? 0 : -1;
}

void login_net_stop(void) {
    net_reactor_stop(g_reactor);
    g_reactor = NULL;
    g_ctx = NULL;
}

void login_net_submit(int fd) {
    if (!g_reactor) {
        LOG_ERROR("[LOGIN] fd %d submitted before the event loops started", fd);
        return;
    }
    net_reactor_submit(g_reactor, fd);
}

int login_net_connection_count(void) {
    return net_reactor_connection_count(g_reactor);
}
