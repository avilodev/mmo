/**
 * @file
 * Authenticate realm-to-world connections and load configured world endpoints.
 *
 * The link is mutually authenticated TLS. The realm presents the certificate it
 * also serves clients with, the world presents its own, and each checks the
 * other against a pin -- there is no CA in this system, so a pin is the only
 * thing that separates the real world server from anything else that answers on
 * that port. What travels here is the shared server auth key, which is what
 * makes a world willing to mint world-entry tickets at all.
 */
#include "world_connect.h"
#include "log.h"
#include "world_table.h"
#include "tls.h"
#include "config.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdlib.h>

/* send_exact() and recv_exact_timeout() used to live here, writing to a bare
 * descriptor. Their TLS equivalents are tls_send_exact() and tls_recv_exact()
 * in common/src/tls.c, which additionally know that one TLS record can carry
 * several protocol packets -- so they check SSL_pending() before polling,
 * rather than waiting out a timeout on bytes that already arrived. */

/** The realm's own identity, presented to every world it connects to. */
static SSL_CTX*  g_world_ctx = NULL;
/** The world public keys this realm will talk to. */
static CertPinSet g_world_pins;

int world_connect_tls_init(const char* cert_path, const char* key_path,
                           const char* world_pin_path) {
    char reason[256];
    cert_pin_reset(&g_world_pins);

    if (!cert_pin_load_file(&g_world_pins, world_pin_path, reason, sizeof(reason))) {
        LOG_ERROR("No usable world public-key pins in %s: %s", world_pin_path, reason);
        LOG_INFO("The realm will not connect to any world without them. Produce a");
        LOG_INFO("line for a world's certificate with:");
        LOG_INFO("  openssl x509 -in world.crt -pubkey -noout \\");
        LOG_INFO("    | openssl pkey -pubin -outform der \\");
        LOG_INFO("    | openssl dgst -sha256 -binary | openssl base64");
        LOG_INFO("and write it as 'sha256/<base64>' into %s", world_pin_path);
        return 0;
    }

    g_world_ctx = tls_client_ctx_init(cert_path, key_path);
    if (!g_world_ctx) {
        LOG_ERROR("Could not load the realm certificate (%s) for world links", cert_path);
        cert_pin_reset(&g_world_pins);
        return 0;
    }

    LOG_INFO("Realm↔world TLS ready (%d world key%s pinned)",
             g_world_pins.count, g_world_pins.count == 1 ? "" : "s");
    return 1;
}

void world_connect_tls_cleanup(void) {
    if (g_world_ctx) {
        SSL_CTX_free(g_world_ctx);
        g_world_ctx = NULL;
    }
    cert_pin_reset(&g_world_pins);
}

/**
 * Connect with a deadline, leaving the descriptor in blocking mode on success.
 *
 * The socket is switched to non-blocking only for the connect itself, so the
 * kernel returns EINPROGRESS instead of parking this thread in the TCP SYN
 * retry schedule. Everything after the connect is written against a blocking
 * descriptor, so blocking mode is restored before returning.
 *
 * @return 0 when connected, or -1 on error or timeout.
 */
static int connect_with_deadline(int fd, const struct sockaddr* addr,
                                 socklen_t addr_len, int timeout_ms) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) return -1;

    int result = connect(fd, addr, addr_len);
    if (result < 0 && errno == EINPROGRESS) {
        struct pollfd pfd = {.fd = fd, .events = POLLOUT};
        int ready;
        do {
            ready = poll(&pfd, 1, timeout_ms);
        } while (ready < 0 && errno == EINTR);

        if (ready <= 0) {
            /* Timeout is the case this whole function exists for: report it as
             * one rather than as whatever errno poll happened to leave. */
            if (ready == 0) errno = ETIMEDOUT;
            return -1;
        }

        int error = 0;
        socklen_t error_len = sizeof(error);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &error_len) < 0) return -1;
        if (error != 0) {
            errno = error;
            return -1;
        }
        result = 0;
    }

    if (result < 0) return -1;
    if (fcntl(fd, F_SETFL, flags) < 0) return -1;
    return 0;
}

/**
 * Connect to a world endpoint, handshake, and complete server-key authentication.
 *
 * @param out_tls     Receives the session on success.
 * @param silent      Nonzero to suppress connection diagnostics.
 * @param timeout_ms  Deadline applied to each blocking phase, in milliseconds.
 * @return            The authenticated socket descriptor, or -1 on failure.
 */
int connect_to_world_server(const char* host, int port, const char* world_name,
                            const char* server_key, SSL** out_tls,
                            int silent, int timeout_ms) {
    if (timeout_ms <= 0) timeout_ms = 5000;
    if (out_tls) *out_tls = NULL;

    if (!g_world_ctx) {
        if (!silent)
            LOG_ERROR("Realm↔world TLS was never initialized; refusing to connect to %s",
                      world_name ? world_name : host);
        return -1;
    }

    /* getaddrinfo rather than inet_pton so a world may be configured by
     * hostname. AI_NUMERICSERV keeps the service string from being looked up
     * in /etc/services. */
    char port_text[16];
    snprintf(port_text, sizeof(port_text), "%d", port);

    struct addrinfo hints = {0};
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags    = AI_NUMERICSERV;

    struct addrinfo* candidates = NULL;
    int lookup = getaddrinfo(host, port_text, &hints, &candidates);
    if (lookup != 0) {
        if (!silent)
            LOG_ERROR("Cannot resolve world address %s:%d: %s",
                      host, port, gai_strerror(lookup));
        return -1;
    }

    int sockfd = -1;
    for (struct addrinfo* c = candidates; c; c = c->ai_next) {
        sockfd = socket(c->ai_family, c->ai_socktype, c->ai_protocol);
        if (sockfd < 0) continue;

        if (connect_with_deadline(sockfd, c->ai_addr, c->ai_addrlen, timeout_ms) == 0) break;

        if (!silent)
            LOG_ERROR("Connect to world %s:%d failed: %s",
                      host, port, strerror(errno));
        close(sockfd);
        sockfd = -1;
    }
    freeaddrinfo(candidates);

    if (sockfd < 0) return -1;

    /* The pin check happens inside here, before a single byte of protocol is
     * written. Sending the server auth key first and identifying the peer
     * afterwards would hand the key to whatever answered. */
    SSL* ssl = tls_connect_pinned(g_world_ctx, sockfd, &g_world_pins,
                                  world_name ? world_name : host, timeout_ms);
    if (!ssl) {
        if (!silent)
            LOG_WARN("TLS to world %s:%d failed or the peer was not pinned",
                     host, port);
        close(sockfd);
        return -1;
    }

    // Authenticate with server key
    RealmAuthPacket auth = {0};
    auth.header.type = PACKET_REALM_AUTH;
    auth.header.player_id = 0;
    auth.header.payload_size = 0;
    snprintf(auth.server_key, sizeof(auth.server_key), "%s", server_key);
    /* The field existed and was never filled, so every world logged the realm
     * that authenticated to it as ''. Two realms are configured in this tree;
     * telling them apart in a world's log is the whole point of the field. */
    snprintf(auth.realm_name, sizeof(auth.realm_name), "%s", g_server.name);

    if (!tls_send_exact(ssl, &auth, sizeof(auth), timeout_ms)) {
        if (!silent) LOG_ERROR("Could not send auth to %s:%d", host, port);
        tls_close(ssl);
        close(sockfd);
        return -1;
    }

    // Wait for auth response
    RealmAuthAckPacket ack;
    if (!tls_recv_exact(ssl, &ack, sizeof(ack), timeout_ms)) {
        if (!silent) LOG_ERROR("No auth acknowledgement from %s:%d", host, port);
        tls_close(ssl);
        close(sockfd);
        return -1;
    }

    if (ack.header.type != PACKET_REALM_AUTH_ACK || !ack.success) {
        /* Wire-supplied and not necessarily terminated. */
        ack.message[sizeof(ack.message) - 1] = '\0';
        if (!silent) LOG_INFO("Auth failed: %s", ack.message);
        tls_close(ssl);
        close(sockfd);
        return -1;
    }

    if (!silent) {
        ack.message[sizeof(ack.message) - 1] = '\0';
        LOG_INFO("Authenticated to world server over TLS: %s", ack.message);
    }

    if (out_tls) *out_tls = ssl;
    else         tls_close(ssl);   /* a caller that wants only the check */
    return sockfd;
}

/**
 * Populate a world-server array from the loaded world table.
 *
 * @return      The number of entries written, or -1 when no world table could be loaded.
 */
int load_world_servers_from_table(WorldServer* servers, int max_servers) {
    if (!servers || max_servers <= 0) return -1;

    size_t configured = world_table_count();
    if (configured == 0) {
        LOG_ERROR("No worlds configured. The realm has nothing to monitor.");
        return -1;
    }
    if (configured > (size_t)max_servers) {
        LOG_ERROR("World roster holds %zu worlds but only %d fit; "
                          "monitoring the first %d",
                  configured, max_servers, max_servers);
        configured = (size_t)max_servers;
    }

    for (size_t i = 0; i < configured; i++) {
        const WorldEntry* entry = world_table_at(i);
        WorldServer*      ws    = &servers[i];

        memset(ws, 0, sizeof(*ws));
        snprintf(ws->name,   sizeof(ws->name),   "%s", entry->name);
        snprintf(ws->region, sizeof(ws->region), "%s", entry->region);
        snprintf(ws->host,   sizeof(ws->host),   "%s", entry->host);
        ws->port       = entry->port;
        /* The heartbeat link has its own port on the world side; the client
         * port is what goes out in the world list. Both come from the same
         * table row, so the two ends cannot drift apart. */
        ws->realm_port = entry->realm_port;
        ws->fd   = -1;
        /* 0 means "this world has not told us its capacity yet".
         *
         * This was a hardcoded 1000 -- a third number in a system that already
         * had two (the world's configured max_players and the compiled
         * MAX_PLAYERS it is checked against). The realm marks a world online
         * the moment the TCP connect succeeds, which is before the first status
         * heartbeat arrives, so that literal was published to clients as the
         * world's real capacity during the gap. Callers must treat 0 as unknown
         * rather than as a capacity of zero. */
        ws->max_players = 0;

        LOG_INFO("Configured world: %s (%s) at %s:%u (realm link on %u)",
                 ws->name, ws->region, ws->host, (unsigned)ws->port,
                 (unsigned)ws->realm_port);
    }

    LOG_INFO("Loaded %zu world servers from the world table", configured);
    return (int)configured;
}
