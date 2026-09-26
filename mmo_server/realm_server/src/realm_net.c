/**
 * @file
 * Teach the generic reactor what a realm connection is.
 *
 * Two phases, and both of them block. A connection first has to have its
 * session validated against Redis, and after that every packet it sends is a
 * character list, creation or deletion -- all PostgreSQL. So unlike the world
 * server, where only the ends of a session block, here the ordinary path does:
 * a realm packet is handed to a worker, answered there, and the connection
 * returns to its loop for the next one.
 *
 * The connection is TLS, arranged exactly as the login server arranges its
 * own: a transport plugged into the reactor, the handshake driven across
 * events, and the reply paths reaching their session through a thread-local
 * bound around dispatch. The few answers written from the loop rather than a
 * worker name their session explicitly, because the loop holds many.
 */

#include "realm_net.h"

#include "log.h"
#include "net_notify.h"
#include "net_reactor.h"
#include "packet_limiter.h"
#include "players_database.h"
#include "protocol.h"
#include "routes.h"
#include "session.h"
#include "peer_addr.h"
#include "tls.h"
#include "types.h"
#include "world_database_manager.h"

#include <arpa/inet.h>
#include <string.h>
#include <sys/socket.h>

/* Blocking pool depth, derived from the resource it actually contends for.
 *
 * Every realm packet is a PostgreSQL round trip, and every one of those needs
 * a connection from the pool for the world it concerns. Running more workers
 * than there are connections does not make the database faster; it makes the
 * surplus wait inside the pool's acquire, which gives up after five seconds
 * and fails the query. So the queue is kept in front, in the reactor, where
 * waiting is free and bounded, rather than behind, where it has a deadline.
 *
 * It is the *realm's* per-world pool that is contended here, not the world
 * server's own character pool -- this process never opens one of those. It was
 * derived from DB_CONN_POOL_SIZE, which is that other pool's default, so
 * raising the number this actually waits on ($MMO_WORLD_POOL_SIZE) left the
 * worker count where it was and lowering it left workers to time out.
 * world_database_manager_conn_per_world() is the number in play, and it reads
 * the same environment the pools do. */
/** Silence allowed before a connection has proved who it is. */
#define AUTH_TIMEOUT_SECS   15

/** Hold one realm connection's TLS session and authenticated identity. */
typedef struct {
    SSL*     ssl;
    uint32_t account_id;
    int      authenticated;
} RealmConn;

static SSL_CTX*    g_ctx     = NULL;
static NetReactor* g_reactor = NULL;

static RealmConn* realm_conn(NetReactorConn* conn) {
    return (RealmConn*)net_reactor_conn_user(conn);
}

/** Send a framed disconnect reason down a named session.
 *
 * Named rather than thread-bound because one of the two callers is the loop,
 * which refuses an oversized frame before any worker has been involved.
 */
static void send_disconnect(SSL* ssl, uint8_t reason, const char* message) {
    uint8_t buf[sizeof(DisconnectPacket)];
    size_t n = net_build_disconnect(buf, sizeof(buf), reason, message);
    if (n) tls_send_on(ssl, buf, n);
}

/* --- The TLS transport --------------------------------------------------- */

static int realm_handshake(NetReactorConn* conn) {
    return tls_handshake_continue(realm_conn(conn)->ssl);
}

static ssize_t realm_recv(NetReactorConn* conn, void* buf, size_t len) {
    return tls_read(realm_conn(conn)->ssl, buf, len);
}

/** Release the session. Tolerates a connection that never got one. */
static void realm_transport_close(NetReactorConn* conn) {
    RealmConn* rc = realm_conn(conn);
    if (!rc || !rc->ssl) return;
    tls_close(rc->ssl);
    rc->ssl = NULL;
}

static const NetReactorTransport REALM_TLS = {
    .handshake = realm_handshake,
    .recv      = realm_recv,
    .close     = realm_transport_close,
};

/* --- Accepting ----------------------------------------------------------- */

/** Attach a TLS session, without waiting for the handshake to finish. */
static int realm_on_accept(NetReactorConn* conn) {
    RealmConn* rc = realm_conn(conn);
    int fd = net_reactor_conn_fd(conn);

    rc->ssl = tls_handshake_begin(g_ctx, fd);
    if (!rc->ssl) {
        LOG_ERROR("[REALM] could not start a TLS session for fd %d", fd);
        return -1;
    }

    packet_limiter_reset(fd);
    LOG_DEBUG("[REALM] client connected: fd %d", fd);
    return 0;
}

static void realm_on_reap(NetReactorConn* conn) {
    /* Release the budget so a recycled descriptor never inherits it. */
    packet_limiter_reset(net_reactor_conn_fd(conn));
}

/* --- Reading ------------------------------------------------------------- */

/**
 * Decide whether there is work for a worker, without doing any of it.
 *
 * Runs on the loop, so the only judgements made here are the ones that cost
 * nothing: is a whole packet present, and is it a plausible size.
 */
static NetReactorVerdict realm_on_data(NetReactorConn* conn, uint8_t* data,
                                       size_t len, size_t* consumed) {
    RealmConn* rc = realm_conn(conn);

    /* Nothing is consumed on either path: the worker is given the bytes where
     * they lie, and consumes exactly what it answers. */
    *consumed = 0;

    if (!rc->authenticated)
        return len >= sizeof(RealmConnectPacket) ? NET_REACTOR_TO_WORKER
                                                 : NET_REACTOR_KEEP;

    if (len < sizeof(PacketHeader)) return NET_REACTOR_KEEP;

    const PacketHeader* header = (const PacketHeader*)data;
    size_t packet_size = sizeof(PacketHeader) + ntohs(header->payload_size);

    if (packet_size > MAX_PACKET_SIZE) {
        LOG_WARN("[REALM] fd %d sent an oversized packet (%zu bytes) — disconnecting",
                 net_reactor_conn_fd(conn), packet_size);
        send_disconnect(rc->ssl, DISCONNECT_REASON_PROTOCOL, NULL);
        return NET_REACTOR_CLOSE;
    }

    if (len < packet_size) return NET_REACTOR_KEEP;   /* incomplete */
    return NET_REACTOR_TO_WORKER;
}

/* --- Authenticating ------------------------------------------------------ */

/**
 * Validate a session key against Redis and answer the client.
 *
 * @return NET_REACTOR_KEEP once authenticated, or NET_REACTOR_CLOSE.
 */
static NetReactorVerdict authenticate(NetReactorConn* conn, RealmConn* rc,
                                      const RealmConnectPacket* pkt) {
    int fd = net_reactor_conn_fd(conn);

    if (pkt->header.type != PACKET_REALM_CONNECT) {
        LOG_WARN("[REALM] fd %d opened with packet type %u rather than a session",
                 fd, pkt->header.type);
    } else {
        /* Version before session. A client that disagrees about the wire layout
         * would have its session key read from the wrong offset, and the failure
         * would surface as "invalid session" -- the one message guaranteed to
         * send the player looking in the wrong place. */
        uint16_t client_protocol = ntohs(pkt->protocol_version);
        if (client_protocol != PROTOCOL_VERSION) {
            LOG_WARN("[REALM] fd %d speaks protocol %u, this realm speaks %u — refusing",
                     fd, client_protocol, (unsigned)PROTOCOL_VERSION);
            send_disconnect(rc->ssl, DISCONNECT_REASON_VERSION,
                            "This client is a different version than the server. "
                            "Please update.");
            return NET_REACTOR_CLOSE;
        }

        uint32_t account_id = ntohl(pkt->header.player_id);

        /* Bound to the address the login server issued the session to.
         *
         * The link is TLS now, so the key is no longer readable off the wire --
         * but this check is not about the wire. The same key is minted by the
         * login server and repeated by the client on every realm connect, so
         * anywhere it leaks from (a log, a crash dump, the client's own memory)
         * it must still be useless from another address. */
        char peer[PEER_ADDR_MAXLEN] = {0};
        peer_addr_text(fd, peer, sizeof(peer));

        if (session_validate_from(account_id, pkt->header.session_key, peer)) {
            rc->account_id    = account_id;
            rc->authenticated = 1;

            RealmConnectAckPacket response = {0};
            response.header.type = PACKET_REALM_CONNECT_ACK;
            response.header.player_id = htonl(account_id);
            response.header.payload_size = htons(sizeof(response) - sizeof(PacketHeader));
            response.success = 1;
            strncpy(response.message, "Welcome to Realm Server",
                    sizeof(response.message) - 1);
            if (tls_send_on(rc->ssl, &response, sizeof(response)) != (ssize_t)sizeof(response))
                LOG_WARN("[REALM] fd %d: partial or failed auth ack", fd);

            LOG_INFO("[REALM] account %u authenticated", account_id);

            /* Only the session packet is consumed. Anything the client sent
             * behind it is delivered as soon as the loop takes over again. */
            net_reactor_conn_consume(conn, sizeof(RealmConnectPacket));
            return NET_REACTOR_KEEP;
        }

        LOG_WARN("[REALM] fd %d: session validation failed for account %u", fd, account_id);
    }

    RealmConnectAckPacket response = {0};
    response.header.type = PACKET_REALM_CONNECT_ACK;
    response.header.payload_size = htons(sizeof(response) - sizeof(PacketHeader));
    response.success = 0;
    strncpy(response.message, "Invalid session", sizeof(response.message) - 1);
    tls_send_on(rc->ssl, &response, sizeof(response));
    return NET_REACTOR_CLOSE;
}

/**
 * Run one blocking request: the session handshake, or one realm packet.
 */
static NetReactorVerdict realm_on_work(NetReactorConn* conn) {
    RealmConn* rc = realm_conn(conn);

    size_t len = 0;
    uint8_t* buf = net_reactor_conn_buffer(conn, &len);

    if (!rc->authenticated) {
        if (len < sizeof(RealmConnectPacket)) return NET_REACTOR_CLOSE;
        NetReactorVerdict verdict = authenticate(conn, rc, (const RealmConnectPacket*)buf);
        /* An ack that could not be written leaves the session unwritable. Keeping
         * the connection would hand the player a realm that answers nothing. */
        return tls_write_broken(rc->ssl) ? NET_REACTOR_CLOSE : verdict;
    }

    if (len < sizeof(PacketHeader)) return NET_REACTOR_KEEP;

    const PacketHeader* header = (const PacketHeader*)buf;
    size_t packet_size = sizeof(PacketHeader) + ntohs(header->payload_size);
    if (packet_size > MAX_PACKET_SIZE || len < packet_size) return NET_REACTOR_CLOSE;

    /* One packet per visit. Every realm request is a database round trip, so
     * holding a worker for a whole buffer of them would let one client occupy
     * a worker for as long as it cared to keep writing. */
    /* Every reply below this line is written by a route handler that was given
     * a bare descriptor and nothing else. Bind the session to this worker so
     * those writes find it, and unbind afterwards: the same worker goes on to
     * serve other connections, and a stale binding would answer one client on
     * another's session. */
    tls_set_conn(rc->ssl);
    int result = process_packet(net_reactor_conn_fd(conn), rc->account_id,
                                buf, (ssize_t)packet_size);
    tls_set_conn(NULL);
    net_reactor_conn_consume(conn, packet_size);

    if (result < 0) return NET_REACTOR_CLOSE;

    /* Route handlers do not check what their replies returned -- they were
     * written against send(2) and kept that shape. So the one place that can
     * see a reply having failed is here, after dispatch: a session whose write
     * was abandoned mid-record can carry nothing more, and the next request on
     * it would be answered into a void. Close it while there is still a reason
     * to put in the log. */
    if (tls_write_broken(rc->ssl)) {
        LOG_WARN("[REALM] fd %d: reply could not be written — closing",
                 net_reactor_conn_fd(conn));
        return NET_REACTOR_CLOSE;
    }

    return NET_REACTOR_KEEP;
}

/* --- Leaving ------------------------------------------------------------- */

static void realm_on_retire(NetReactorConn* conn) {
    RealmConn* rc = realm_conn(conn);
    if (rc->authenticated)
        LOG_INFO("[REALM] account %u disconnected (fd=%d)",
                 rc->account_id, net_reactor_conn_fd(conn));
}

/**
 * Drop a connection that opened and then said nothing.
 *
 * Only the unauthenticated phase is timed. A player sitting on the character
 * screen is legitimately silent for as long as they like, and disconnecting
 * them would be a regression on the thread-per-connection version this
 * replaces; an unauthenticated socket is a slot held by someone who has not yet
 * shown they are entitled to one.
 */
static int realm_on_idle(NetReactorConn* conn, long idle_seconds) {
    RealmConn* rc = realm_conn(conn);
    if (rc->authenticated) return 0;

    if (idle_seconds > AUTH_TIMEOUT_SECS) {
        LOG_INFO("[TIMEOUT] fd=%d never authenticated (%lds) — dropping",
                 net_reactor_conn_fd(conn), idle_seconds);
        return 1;
    }
    return 0;
}

/* --- Lifecycle ----------------------------------------------------------- */

int realm_net_start(SSL_CTX* ctx, int worker_count) {
    if (!ctx) return -1;
    g_ctx = ctx;

    NetReactorConfig config;
    memset(&config, 0, sizeof(config));

    config.name         = "REALM";
    config.worker_count = worker_count > 0
        ? worker_count
        : world_database_manager_conn_per_world();
    config.buffer_size  = MAX_PACKET_SIZE * 2;
    config.user_size    = sizeof(RealmConn);
    config.transport    = &REALM_TLS;

    config.on_accept    = realm_on_accept;
    config.on_data      = realm_on_data;
    config.on_work      = realm_on_work;
    config.on_retire    = realm_on_retire;
    config.on_reap      = realm_on_reap;
    config.on_idle      = realm_on_idle;

    g_reactor = net_reactor_start(&config);
    return g_reactor ? 0 : -1;
}

void realm_net_stop(void) {
    net_reactor_stop(g_reactor);
    g_reactor = NULL;
    g_ctx = NULL;
}

void realm_net_submit(int fd) {
    if (!g_reactor) {
        LOG_ERROR("[REALM] fd %d submitted before the event loops started", fd);
        return;
    }
    net_reactor_submit(g_reactor, fd);
}

int realm_net_connection_count(void) {
    return net_reactor_connection_count(g_reactor);
}
