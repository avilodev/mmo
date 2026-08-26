/**
 * @file
 * Drive realm and world connections without stopping the frame.
 */

#include "net_internal.h"
#include "net_connect.h"
#include "network/net_tls.h"

#include <stdio.h>
#include <string.h>

/** Seconds allowed for the TCP connect itself. */
#define CONNECT_TIMEOUT_SECONDS 5.0

/** Seconds allowed for the server's acknowledgement after the connect packet. */
#define REALM_ACK_TIMEOUT_SECONDS 10.0
#define WORLD_ACK_TIMEOUT_SECONDS 5.0

/** Seconds allowed for the realm's TLS handshake.
 *
 * Generous next to the connect: the handshake is several round trips plus a
 * signature verification, and a client on a slow link should not be told the
 * realm is down because its own certificate check took four seconds. */
#define TLS_HANDSHAKE_TIMEOUT_SECONDS 10.0

/** Which handshake an attempt is running. */
typedef enum { TARGET_NONE = 0, TARGET_REALM, TARGET_WORLD } ConnectTarget;

/** Internal phases, finer than the ones callers see. */
typedef enum {
    STAGE_IDLE = 0,
    STAGE_CONNECTING,     /**< Non-blocking connect in flight. */
    STAGE_HANDSHAKING,    /**< TCP up; TLS handshake in flight. Realm only. */
    STAGE_AWAITING_ACK    /**< Connect packet sent; waiting for the answer. */
} ConnectStage;

static struct {
    ConnectTarget target;
    ConnectStage  stage;
    double        deadline;      /**< net_now() past which this stage has failed. */
    uint32_t      id;            /**< account_id for a realm, character_id for a world. */

    /* The connect packet, built up front and sent once the socket is writable. */
    union {
        RealmConnectPacket realm;
        WorldConnectPacket world;
    } packet;
    int packet_size;

    char message[128];
} g_attempt;

/** Record an outcome and release the socket. */
static void finish_failed(const char* why) {
    snprintf(g_attempt.message, sizeof(g_attempt.message), "%s", why);
    NET_WARN("[NET] %s\n", why);

    g_net.connected   = FALSE;
    g_net.handshaking = FALSE;
    net_tls_close();
    if (g_net.socket != INVALID_SOCKET) {
        closesocket(g_net.socket);
        g_net.socket = INVALID_SOCKET;
    }
    g_attempt.stage  = STAGE_IDLE;
    g_attempt.target = TARGET_NONE;
}

/** Record a success, leaving the socket live. */
static void finish_succeeded(const char* what) {
    snprintf(g_attempt.message, sizeof(g_attempt.message), "%s", what);

    g_net.connected      = TRUE;
    g_net.handshaking    = FALSE;   /* the ack landed; ordinary traffic is now ours */
    g_net.pending_pings  = 0;
    g_net.last_ping_time = net_now();

    g_attempt.stage  = STAGE_IDLE;
    g_attempt.target = TARGET_NONE;
}

/**
 * Open a non-blocking socket and start connecting.
 *
 * Non-blocking is set BEFORE connect, not after: setting it afterwards is what
 * made this function block for the operating system's full SYN timeout on an
 * address that was filtered rather than refused.
 *
 * @return 1 when the connect is in flight or already complete, otherwise 0.
 */
static int start_socket(const char* ip, uint16_t port) {
    /* Any TLS session belongs to the descriptor being replaced, so it goes
     * first. network_connect_abort() cannot be relied on for this: it returns
     * early when no attempt is in flight, which is exactly the state a live
     * realm session is in when the player picks a world -- and a session left
     * attached would then encrypt the world's plaintext traffic into a socket
     * that no longer exists. */
    net_tls_close();

    if (g_net.socket != INVALID_SOCKET) {
        closesocket(g_net.socket);
        g_net.socket = INVALID_SOCKET;
    }

    g_net.socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (g_net.socket == INVALID_SOCKET) {
        finish_failed("Could not create a socket");
        return 0;
    }

    u_long nonblocking = 1;
    ioctlsocket(g_net.socket, FIONBIO, &nonblocking);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);

    if (inet_pton(AF_INET, ip, &addr.sin_addr) <= 0) {
        char why[128];
        snprintf(why, sizeof(why), "Invalid address: %.60s", ip);
        finish_failed(why);
        return 0;
    }

    if (connect(g_net.socket, (struct sockaddr*)&addr, sizeof(addr)) == 0)
        return 1;   // connected immediately, which loopback often does

#ifdef _WIN32
    int err = WSAGetLastError();
    if (err == WSAEWOULDBLOCK || err == WSAEINPROGRESS) return 1;
#else
    if (errno == EINPROGRESS || errno == EWOULDBLOCK) return 1;
#endif

    finish_failed("Could not reach the server");
    return 0;
}

/**
 * Start connecting to a realm server.
 *
 * @return 1 when the attempt started, otherwise 0.
 */
int network_begin_realm_connect(const char* ip, uint16_t port,
                                const char* session_key, uint32_t account_id) {
    if (!g_net.initialized) return 0;

    network_connect_abort();

    g_net.recv_len = 0;                     // no bytes carry over between sessions
    g_net.realm_connect_ack.ready = FALSE;

    memset(&g_attempt, 0, sizeof(g_attempt));
    g_attempt.target = TARGET_REALM;
    g_attempt.id     = account_id;

    RealmConnectPacket* pkt = &g_attempt.packet.realm;
    memset(pkt, 0, sizeof(*pkt));
    pkt->header.type         = PACKET_REALM_CONNECT;
    pkt->header.player_id    = htonl(account_id);
    pkt->header.payload_size = 0;
    memcpy(pkt->header.session_key, session_key, 32);
    // Stated up front so a version mismatch is refused by the realm as a
    // version mismatch, instead of surfacing as an invalid session.
    pkt->protocol_version = htons(PROTOCOL_VERSION);
    g_attempt.packet_size = (int)sizeof(*pkt);

    NET_LOG("[NET] Connecting to realm %s:%u...\n", ip, port);

    if (!start_socket(ip, port)) return 0;

    g_attempt.stage    = STAGE_CONNECTING;
    g_attempt.deadline = net_now() + CONNECT_TIMEOUT_SECONDS;
    return 1;
}

/**
 * Start connecting to a world server, replacing any current connection.
 *
 * @return 1 when the attempt started, otherwise 0.
 */
int network_begin_world_connect(const char* ip, uint16_t port,
                                const char* game_ticket, uint32_t character_id) {
    if (!g_net.initialized) return 0;

    network_connect_abort();

    g_net.recv_len = 0;
    g_net.world_connect_ack.ready = FALSE;
    g_net.character_id = character_id;

    memset(&g_attempt, 0, sizeof(g_attempt));
    g_attempt.target = TARGET_WORLD;
    g_attempt.id     = character_id;

    WorldConnectPacket* pkt = &g_attempt.packet.world;
    memset(pkt, 0, sizeof(*pkt));
    pkt->header.type         = PACKET_WORLD_CONNECT;
    pkt->header.player_id    = htonl(character_id);
    pkt->header.payload_size = htons(sizeof(WorldConnectPacket) - sizeof(PacketHeader));
    memcpy(pkt->game_ticket, game_ticket, 64);
    pkt->character_id    = htonl(character_id);
    pkt->protocol_version = htons(PROTOCOL_VERSION);
    g_attempt.packet_size = (int)sizeof(*pkt);

    NET_LOG("[NET] Connecting to world %s:%u...\n", ip, port);

    if (!start_socket(ip, port)) return 0;

    g_attempt.stage    = STAGE_CONNECTING;
    g_attempt.deadline = net_now() + CONNECT_TIMEOUT_SECONDS;
    return 1;
}

/** Report whether the socket has finished connecting.
 *
 * @return 1 when writable and healthy, 0 when still waiting, -1 on failure.
 */
static int connect_finished(void) {
    fd_set writable, failed;
    FD_ZERO(&writable);
    FD_ZERO(&failed);
    FD_SET(g_net.socket, &writable);
    FD_SET(g_net.socket, &failed);

    struct timeval no_wait = { 0, 0 };
    int ready = select((int)g_net.socket + 1, NULL, &writable, &failed, &no_wait);
    if (ready <= 0) return 0;

    if (FD_ISSET(g_net.socket, &failed)) return -1;
    if (!FD_ISSET(g_net.socket, &writable)) return 0;

    /* Writable is not the same as connected: a refused connection also makes
     * the descriptor writable, and SO_ERROR is what separates the two. */
    int        so_error = 0;
    socklen_t  len      = sizeof(so_error);
    if (getsockopt(g_net.socket, SOL_SOCKET, SO_ERROR, (char*)&so_error, &len) != 0)
        return -1;

    return so_error == 0 ? 1 : -1;
}

/** Move to STAGE_AWAITING_ACK, arming the flags the read path depends on.
 *
 * network_update() only reads while the context believes it is connected, so
 * the handshake runs with the flag already set; a failure clears it again.
 *
 * `handshaking` is the narrower claim: the socket is up but this is not a
 * session yet, and process_packet() will admit only the acknowledgement and
 * the control opcodes until it is.
 */
static NetConnectPhase begin_awaiting_ack(double now) {
    g_net.connected    = TRUE;
    g_net.handshaking  = TRUE;
    g_attempt.stage    = STAGE_AWAITING_ACK;
    g_attempt.deadline = now + (g_attempt.target == TARGET_REALM
                                ? REALM_ACK_TIMEOUT_SECONDS
                                : WORLD_ACK_TIMEOUT_SECONDS);
    return NET_CONNECT_PENDING;
}

/** Send the connect packet the attempt has been holding. */
static int send_connect_packet(void) {
    int sent = (int)net_send((const char*)&g_attempt.packet, (size_t)g_attempt.packet_size);
    return sent == g_attempt.packet_size;
}

/**
 * Advance the current attempt.
 */
NetConnectPhase network_connect_poll(void) {
    if (g_attempt.stage == STAGE_IDLE) return NET_CONNECT_IDLE;

    double now = net_now();

    if (g_attempt.stage == STAGE_CONNECTING) {
        int state = connect_finished();

        if (state < 0) {
            finish_failed("Connection refused");
            return NET_CONNECT_FAILED;
        }
        if (state == 0) {
            if (now > g_attempt.deadline) {
                finish_failed("Connection timed out");
                return NET_CONNECT_FAILED;
            }
            return NET_CONNECT_PENDING;
        }

        /* The realm link is TLS, so the connect packet cannot go out yet: it
         * carries the session key, and a session key written before the peer
         * has been identified is a session key given to whatever answered. The
         * world link is plaintext and skips straight to sending. */
        if (g_attempt.target == TARGET_REALM) {
            if (!net_tls_begin(g_net.socket)) {
                finish_failed(net_tls_message()[0] ? net_tls_message()
                                                   : "Could not start TLS");
                return NET_CONNECT_FAILED;
            }
            g_attempt.stage    = STAGE_HANDSHAKING;
            g_attempt.deadline = now + TLS_HANDSHAKE_TIMEOUT_SECONDS;
            return NET_CONNECT_PENDING;
        }

        if (!send_connect_packet()) {
            finish_failed("Could not send the connect packet");
            return NET_CONNECT_FAILED;
        }
        return begin_awaiting_ack(now);
    }

    if (g_attempt.stage == STAGE_HANDSHAKING) {
        int state = net_tls_continue();

        if (state < 0) {
            /* Covers a failed handshake and a completed handshake against a
             * key that is not pinned. The second is the one worth having a
             * message for: it is what a player sees when the realm's
             * certificate was rotated and their client was not updated. */
            finish_failed(net_tls_message()[0] ? net_tls_message()
                                               : "TLS handshake failed");
            return NET_CONNECT_FAILED;
        }
        if (state == 0) {
            if (now > g_attempt.deadline) {
                finish_failed("TLS handshake timed out");
                return NET_CONNECT_FAILED;
            }
            return NET_CONNECT_PENDING;
        }

        if (!send_connect_packet()) {
            finish_failed("Could not send the connect packet");
            return NET_CONNECT_FAILED;
        }
        return begin_awaiting_ack(now);
    }

    /* STAGE_AWAITING_ACK: drain whatever has arrived and look for the answer. */
    network_update();

    if (g_attempt.target == TARGET_REALM) {
        if (g_net.realm_connect_ack.ready) {
            g_net.realm_connect_ack.data.message[
                sizeof(g_net.realm_connect_ack.data.message) - 1] = '\0';
            if (!g_net.realm_connect_ack.data.success) {
                char why[128];
                snprintf(why, sizeof(why), "Realm refused the connection: %.80s",
                         g_net.realm_connect_ack.data.message);
                finish_failed(why);
                return NET_CONNECT_FAILED;
            }
            finish_succeeded(g_net.realm_connect_ack.data.message);
            return NET_CONNECT_SUCCEEDED;
        }
    } else {
        if (g_net.world_connect_ack.ready) {
            g_net.world_connect_ack.data.welcome_message[
                sizeof(g_net.world_connect_ack.data.welcome_message) - 1] = '\0';
            if (!g_net.world_connect_ack.data.success) {
                char why[128];
                snprintf(why, sizeof(why), "World refused the connection: %.80s",
                         g_net.world_connect_ack.data.welcome_message);
                finish_failed(why);
                return NET_CONNECT_FAILED;
            }
            finish_succeeded(g_net.world_connect_ack.data.welcome_message);
            return NET_CONNECT_SUCCEEDED;
        }
    }

    /* A socket the server closed during the handshake shows up here, because
     * network_update() clears `connected` when the peer goes away. */
    if (!g_net.connected) {
        finish_failed("The server closed the connection during the handshake");
        return NET_CONNECT_FAILED;
    }

    if (now > g_attempt.deadline) {
        finish_failed("The server did not answer the connect packet");
        return NET_CONNECT_FAILED;
    }

    return NET_CONNECT_PENDING;
}

/** Describe the outcome of the last attempt. */
const char* network_connect_message(void) {
    return g_attempt.message[0] ? g_attempt.message : "";
}

/** Abandon any attempt in flight and close its socket. */
void network_connect_abort(void) {
    if (g_attempt.stage == STAGE_IDLE) return;

    g_net.connected   = FALSE;
    g_net.handshaking = FALSE;
    net_tls_close();
    if (g_net.socket != INVALID_SOCKET) {
        closesocket(g_net.socket);
        g_net.socket = INVALID_SOCKET;
    }
    g_attempt.stage  = STAGE_IDLE;
    g_attempt.target = TARGET_NONE;
}

/** Report whether an attempt is currently in flight. */
int network_connect_in_flight(void) {
    return g_attempt.stage != STAGE_IDLE;
}
