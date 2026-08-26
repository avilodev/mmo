/**
 * @file
 * Check the login server's half of the reactor contract, over real TLS.
 *
 * The whole point of moving login off thread-per-connection is the handshake:
 * it is spent before a byte of protocol is read, and on a patch day everybody
 * pays it at once. So this test uses a real OpenSSL client against the real
 * server path rather than a stand-in. A handshake that only works when a thread
 * can sit inside SSL_accept() would pass any test that faked it, and fail on the
 * first connection in production.
 *
 * route_packet() is stubbed. It runs the password hash and the database, and
 * what is under test is that it is called once, on a worker, with a whole
 * packet and a TLS session bound to that worker's thread.
 */

#include "login_net.h"
#include "log.h"
#include "tls.h"
#include "types.h"
#include "tls_fixture.h"

#include <arpa/inet.h>
#include <signal.h>
#include <assert.h>
#include <netinet/in.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* --- Stubs --------------------------------------------------------------- */

static atomic_int g_routed;

/** Record what route_packet() was handed. */
#define SEEN_CAP 64
static atomic_int g_seen_count;
static struct { uint8_t type; ssize_t bytes; int replied; } g_seen[SEEN_CAP];

/** Answer through the TLS session the worker bound, the way auth.c does. */
void route_packet(int client_fd, uint8_t* buffer, ssize_t bytes) {
    const PacketHeader* header = (const PacketHeader*)buffer;

    int slot = atomic_fetch_add(&g_seen_count, 1);

    /* The reply path reaches its session through a thread-local. If the worker
     * had not bound one, this write goes nowhere -- which is exactly the bug
     * this assertion exists to catch. */
    uint8_t reply[sizeof(PacketHeader) + 4];
    PacketHeader* out = (PacketHeader*)reply;
    out->type         = (uint8_t)(header->type + 1);
    out->player_id    = header->player_id;
    out->payload_size = htons(4);
    memset(reply + sizeof(PacketHeader), 0x5A, 4);

    ssize_t sent = tls_send(client_fd, reply, sizeof(reply), 0);

    if (slot < SEEN_CAP) {
        g_seen[slot].type    = header->type;
        g_seen[slot].bytes   = bytes;
        g_seen[slot].replied = sent == (ssize_t)sizeof(reply);
    }
    atomic_fetch_add(&g_routed, 1);
}

/* --- Scaffolding --------------------------------------------------------- */

static int g_listen_fd = -1, g_listen_port = 0;
static SSL_CTX* g_server_ctx = NULL;
static TlsFixtureIdentity g_login_id;
static SSL_CTX* g_client_ctx = NULL;
static atomic_int passed;

static void check(int condition, const char* what) {
    if (!condition) {
        printf("  FAIL: %s\n", what);
        exit(1);
    }
    atomic_fetch_add(&passed, 1);
}

static void counters_reset(void) {
    atomic_store(&g_routed, 0);
    atomic_store(&g_seen_count, 0);
    memset(g_seen, 0, sizeof(g_seen));
}

static void start_listener(void) {
    g_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(g_listen_fd >= 0);
    int opt = 1;
    setsockopt(g_listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(bind(g_listen_fd, (struct sockaddr*)&addr, sizeof(addr)) == 0);
    assert(listen(g_listen_fd, 512) == 0);

    socklen_t len = sizeof(addr);
    assert(getsockname(g_listen_fd, (struct sockaddr*)&addr, &len) == 0);
    g_listen_port = ntohs(addr.sin_port);
}

/** One client: a socket, its TLS session, and whether the handshake finished. */
typedef struct {
    int  fd;
    SSL* ssl;
} Client;

/** Connect, hand the peer to the reactor, and open a TLS session over it. */
static Client connect_client(void) {
    Client client = {0};

    client.fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(client.fd >= 0);

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((uint16_t)g_listen_port);
    assert(connect(client.fd, (struct sockaddr*)&addr, sizeof(addr)) == 0);

    int peer = accept(g_listen_fd, NULL, NULL);
    assert(peer >= 0);
    login_net_submit(peer);

    client.ssl = SSL_new(g_client_ctx);
    assert(client.ssl != NULL);
    SSL_set_fd(client.ssl, client.fd);
    return client;
}

static void close_client(Client* client) {
    if (client->ssl) { SSL_free(client->ssl); client->ssl = NULL; }
    if (client->fd >= 0) { close(client->fd); client->fd = -1; }
}

/** Drive the client side of the handshake to completion. */
static int handshake(Client* client) {
    struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
    setsockopt(client->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(client->fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    return SSL_connect(client->ssl) == 1;
}

static int wait_for(atomic_int* counter, int target, int millis) {
    for (int i = 0; i < millis; i++) {
        if (atomic_load(counter) >= target) return 1;
        usleep(1000);
    }
    return 0;
}

static size_t build_packet(uint8_t* out, uint8_t type, uint16_t payload) {
    PacketHeader* header = (PacketHeader*)out;
    header->type         = type;
    header->player_id    = htonl(77);
    header->payload_size = htons(payload);
    memset(out + sizeof(PacketHeader), 0xC3, payload);
    return sizeof(PacketHeader) + payload;
}

/* --- Tests --------------------------------------------------------------- */

static void test_a_handshake_completes_against_an_event_loop(void) {
    printf("a real TLS handshake completes without a thread sitting inside it\n");
    counters_reset();

    Client client = connect_client();
    check(handshake(&client), "the handshake finished");
    check(SSL_get_cipher(client.ssl) != NULL, "and negotiated a cipher");

    close_client(&client);
}

static void test_one_request_is_answered_and_the_connection_closes(void) {
    printf("one request is routed, answered through TLS, and the connection ends\n");
    counters_reset();

    Client client = connect_client();
    check(handshake(&client), "handshake");

    uint8_t pkt[64];
    size_t n = build_packet(pkt, 3, 8);       /* PACKET_AUTH_LOGIN */
    check(SSL_write(client.ssl, pkt, (int)n) == (int)n, "the request went out");

    check(wait_for(&g_routed, 1, 3000), "it reached the router");
    check(g_seen[0].type == 3, "as the packet type that was sent");
    check(g_seen[0].bytes == (ssize_t)n, "framed to exactly one packet");
    check(g_seen[0].replied,
          "and the reply went out, so the worker really did bind the session");

    uint8_t reply[64];
    int got = SSL_read(client.ssl, reply, sizeof(reply));
    check(got == (int)(sizeof(PacketHeader) + 4), "the client received the reply");
    check(reply[0] == 4, "with the answering packet type");

    /* The login server answers exactly one packet per connection, and always has. */
    check(SSL_read(client.ssl, reply, sizeof(reply)) <= 0,
          "and the server closed the connection afterwards");

    close_client(&client);
}

static void test_a_request_split_across_writes_is_reassembled(void) {
    printf("a request arriving in pieces is routed once, whole\n");
    counters_reset();

    Client client = connect_client();
    check(handshake(&client), "handshake");

    uint8_t pkt[64];
    size_t n = build_packet(pkt, 6, 16);      /* PACKET_START_GAME_REQUEST */

    /* Header first, then the payload a few bytes at a time, with the loop given
     * every chance to wake on each fragment. */
    check(SSL_write(client.ssl, pkt, (int)sizeof(PacketHeader)) ==
          (int)sizeof(PacketHeader), "the header went out");
    usleep(50000);
    check(atomic_load(&g_routed) == 0, "a header on its own is not a request");

    for (size_t i = sizeof(PacketHeader); i < n; i += 4) {
        int chunk = (int)((n - i) < 4 ? (n - i) : 4);
        check(SSL_write(client.ssl, pkt + i, chunk) == chunk, "a fragment went out");
        usleep(5000);
    }

    check(wait_for(&g_routed, 1, 3000), "the pieces were reassembled");
    check(atomic_load(&g_routed) == 1, "into exactly one request");
    check(g_seen[0].bytes == (ssize_t)n, "of the right length");

    close_client(&client);
}

static void test_an_absurd_length_is_refused(void) {
    printf("a request claiming more than the wire allows is refused\n");
    counters_reset();

    Client client = connect_client();
    check(handshake(&client), "handshake");

    PacketHeader header;
    header.type         = 3;
    header.player_id    = 0;
    header.payload_size = htons((uint16_t)(MAX_PACKET_SIZE + 100));
    check(SSL_write(client.ssl, &header, (int)sizeof(header)) == (int)sizeof(header),
          "the header went out");

    /* Watching the connection count rather than reading: a read that merely
     * times out looks identical to a server that closed, and the failure being
     * guarded against here is the server sitting and waiting for a payload that
     * is never coming. */
    int dropped = 0;
    for (int i = 0; i < 3000 && !dropped; i++) {
        if (login_net_connection_count() == 0) dropped = 1;
        usleep(1000);
    }
    check(dropped, "the server dropped it promptly rather than waiting for the payload");
    check(atomic_load(&g_routed) == 0, "and no worker was spent on it");

    close_client(&client);
}

static void test_a_peer_that_never_speaks_tls_is_dropped(void) {
    printf("a socket that opens and never handshakes does not hold a slot forever\n");
    counters_reset();

    int raw = socket(AF_INET, SOCK_STREAM, 0);
    assert(raw >= 0);
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((uint16_t)g_listen_port);
    assert(connect(raw, (struct sockaddr*)&addr, sizeof(addr)) == 0);

    int peer = accept(g_listen_fd, NULL, NULL);
    assert(peer >= 0);
    login_net_submit(peer);

    usleep(200000);
    check(login_net_connection_count() >= 1, "it is held while it might still speak");

    /* Rubbish that is not a TLS ClientHello: the handshake must fail rather
     * than wait, so a port scanner costs a connection slot and not a thread. */
    const char junk[] = "GET / HTTP/1.1\r\nHost: x\r\n\r\n";
    assert(write(raw, junk, sizeof(junk) - 1) == (ssize_t)(sizeof(junk) - 1));

    int dropped = 0;
    for (int i = 0; i < 3000 && !dropped; i++) {
        if (login_net_connection_count() == 0) dropped = 1;
        usleep(1000);
    }
    check(dropped, "a peer that is not speaking TLS is dropped");
    check(atomic_load(&g_routed) == 0, "and nothing was routed for it");

    close(raw);
}

static void test_many_handshakes_at_once(void) {
    printf("many clients handshake and are answered on a fixed set of threads\n");
    counters_reset();

    enum { CLIENTS = 24 };
    Client clients[CLIENTS];

    for (int i = 0; i < CLIENTS; i++) clients[i] = connect_client();
    for (int i = 0; i < CLIENTS; i++)
        check(handshake(&clients[i]), "every handshake completed");

    for (int i = 0; i < CLIENTS; i++) {
        uint8_t pkt[64];
        size_t n = build_packet(pkt, 3, 8);
        check(SSL_write(clients[i].ssl, pkt, (int)n) == (int)n, "request sent");
    }

    check(wait_for(&g_routed, CLIENTS, 8000), "every request was routed");

    int replied = 0;
    for (int i = 0; i < CLIENTS; i++) if (g_seen[i].replied) replied++;
    check(replied == CLIENTS, "and every one was answered through its own session");

    for (int i = 0; i < CLIENTS; i++) close_client(&clients[i]);
}

int main(void) {
    printf("=== login connections ===\n");
    /* The servers do this in setup_signals(); this test does not link it.
     * Without it, writing to a connection the server has already closed kills
     * the test rather than returning an error the way production sees it. */
    signal(SIGPIPE, SIG_IGN);

    log_init();
    log_set_level(LOG_LEVEL_ERROR);

    /* The certificate is minted for this run rather than read from
     * certs/server.crt. That path made the whole suite skip itself whenever it
     * ran from a directory without one -- including every fresh checkout, and
     * CI until the setup script had run -- so the one test of the login
     * server's transport reported success by not running. */
    g_server_ctx = tls_fixture_server_ctx(&g_login_id, "mmo-login-test");
    g_client_ctx = tls_fixture_client_ctx();

    start_listener();
    check(login_net_start(g_server_ctx, 0) == 0, "the login reactor started");

    test_a_handshake_completes_against_an_event_loop();
    test_one_request_is_answered_and_the_connection_closes();
    test_a_request_split_across_writes_is_reassembled();
    test_an_absurd_length_is_refused();
    test_a_peer_that_never_speaks_tls_is_dropped();
    test_many_handshakes_at_once();

    login_net_stop();
    SSL_CTX_free(g_client_ctx);
    tls_server_cleanup(g_server_ctx);
    tls_fixture_identity_free(&g_login_id);
    close(g_listen_fd);

    printf("\n%d checks passed\n", atomic_load(&passed));
    return 0;
}
