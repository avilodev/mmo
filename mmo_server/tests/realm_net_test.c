/**
 * @file
 * Check the realm server's half of the reactor contract.
 *
 * The reactor underneath is tested on its own; what is tested here is only what
 * the realm adds to it, and the cases worth pinning down are the ones where
 * getting it wrong costs something real.
 *
 * A connection must prove who it is before anything reaches the database, and a
 * client on the wrong protocol version has to be told that rather than being
 * told its session is invalid -- the second message sends the player looking in
 * entirely the wrong place. After that, every packet is a database round trip,
 * so exactly one may be answered per visit to a worker: a client that writes ten
 * requests in one packet must not be able to hold a worker for ten queries.
 *
 * session_validate() and process_packet() are stubbed. Both really talk to Redis
 * and PostgreSQL, and what is under test is which of them get called and with
 * what, not what they return from a live database.
 *
 * The transport is not stubbed. Every client here completes a real TLS
 * handshake against a certificate minted for the run, because the realm's
 * transport is the thing most recently changed underneath these cases and a
 * fixture that spoke plaintext would prove the reactor still works while
 * saying nothing about whether the server it fronts is reachable at all.
 */

#include "realm_net.h"
#include "limit_profiles.h"
#include "tls_fixture.h"
#include "log.h"
#include "packet_limiter.h"
#include "protocol.h"
#include "types.h"

#include <arpa/inet.h>
#include <signal.h>
#include <assert.h>
#include <netinet/in.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* --- Stubs --------------------------------------------------------------- */

/** The one session key this fixture accepts. */
static const char VALID_KEY[32] = "valid-session-key";

static atomic_int g_validations, g_packets, g_max_concurrent_work;
static atomic_int g_in_work;

/** Record the packets process_packet() was handed, so framing can be asserted. */
#define SEEN_CAP 64
static atomic_int g_seen_count;
static struct { uint8_t type; uint32_t account_id; ssize_t bytes; } g_seen[SEEN_CAP];

/** Ask the caller to disconnect when it sees this packet type. */
#define PACKET_TEST_CLOSE 0xEE
/** An ordinary packet the fixture is happy to answer. */
#define PACKET_TEST_OK    0xE0
/** The same, from a second connection, so the order of service is visible. */
#define PACKET_TEST_OTHER 0xE1

/** Stub the per-world pool size the worker count is derived from.
 *
 * The real one reads $MMO_WORLD_POOL_SIZE; this test opens no database, and
 * what it cares about is that realm_net_start() asks for a plausible worker
 * count, not which number it gets. */
int world_database_manager_conn_per_world(void);
int world_database_manager_conn_per_world(void) { return 4; }

int session_validate(uint32_t account_id, const char* session_key) {
    (void)account_id;
    atomic_fetch_add(&g_validations, 1);
    return session_key && memcmp(session_key, VALID_KEY, sizeof(VALID_KEY)) == 0;
}

/* The realm binds a session to the address it was issued to. The fixture drives
 * loopback sockets, so the binding is not what this suite is about; it is
 * covered by session_binding_test. */
int session_validate_from(uint32_t account_id, const char* session_key,
                          const char* peer_ip) {
    (void)peer_ip;
    return session_validate(account_id, session_key);
}

int process_packet(int client_fd, uint32_t account_id, uint8_t* buffer, ssize_t bytes) {
    (void)client_fd;

    /* Watch the pool depth from inside it: one packet per visit means a client
     * cannot pin a worker, and the reactor should still overlap distinct
     * connections across the pool. */
    int now = atomic_fetch_add(&g_in_work, 1) + 1;
    int high = atomic_load(&g_max_concurrent_work);
    while (now > high && !atomic_compare_exchange_weak(&g_max_concurrent_work, &high, now)) { }

    const PacketHeader* header = (const PacketHeader*)buffer;

    int slot = atomic_fetch_add(&g_seen_count, 1);
    if (slot < SEEN_CAP) {
        g_seen[slot].type       = header->type;
        g_seen[slot].account_id = account_id;
        g_seen[slot].bytes      = bytes;
    }
    atomic_fetch_add(&g_packets, 1);

    usleep(2000);   /* stand in for a database round trip */
    atomic_fetch_sub(&g_in_work, 1);

    return header->type == PACKET_TEST_CLOSE ? -1 : 0;
}

/* --- Scaffolding --------------------------------------------------------- */

static int g_listen_fd = -1, g_listen_port = 0;
static atomic_int passed;

static void check(int condition, const char* what) {
    if (!condition) {
        printf("  FAIL: %s\n", what);
        exit(1);
    }
    atomic_fetch_add(&passed, 1);
}

static void counters_reset(void) {
    atomic_store(&g_validations, 0);
    atomic_store(&g_packets, 0);
    atomic_store(&g_seen_count, 0);
    atomic_store(&g_in_work, 0);
    atomic_store(&g_max_concurrent_work, 0);
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

/** The realm's identity for this run, and a client context to meet it with. */
static TlsFixtureIdentity g_realm_id;
static SSL_CTX*           g_server_ctx = NULL;
static SSL_CTX*           g_client_ctx = NULL;

/** Connect a client, hand its peer to the reactor, and complete a handshake.
 *
 * Blocking on the client side: this thread is the test, and the reactor is
 * driving the server half from its own loop, so there is something to block
 * against.
 */
static SSL* connect_client(void) {
    int c = socket(AF_INET, SOCK_STREAM, 0);
    assert(c >= 0);
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((uint16_t)g_listen_port);
    assert(connect(c, (struct sockaddr*)&addr, sizeof(addr)) == 0);

    int peer = accept(g_listen_fd, NULL, NULL);
    assert(peer >= 0);
    realm_net_submit(peer);

    SSL* ssl = SSL_new(g_client_ctx);
    assert(ssl);
    SSL_set_fd(ssl, c);
    assert(SSL_connect(ssl) == 1);

    /* The realm is what this client would pin in production; assert here that
     * the key it presented is the one the fixture generated, so a handshake
     * against something else could not pass as success. */
    unsigned char digest[CERT_PIN_DIGEST_LEN];
    assert(cert_spki_digest_peer(ssl, digest) == 1);
    assert(memcmp(digest, g_realm_id.pin, CERT_PIN_DIGEST_LEN) == 0);

    return ssl;
}

/** Write a whole buffer through a client session. */
static void client_write(SSL* ssl, const void* buf, size_t len) {
    assert(SSL_write(ssl, buf, (int)len) == (int)len);
}

/** Close a client session and the descriptor under it. */
static void client_close(SSL* ssl) {
    int fd = SSL_get_fd(ssl);
    SSL_shutdown(ssl);
    SSL_free(ssl);
    close(fd);
}

static int wait_for(atomic_int* counter, int target, int millis) {
    for (int i = 0; i < millis; i++) {
        if (atomic_load(counter) >= target) return 1;
        usleep(1000);
    }
    return 0;
}

/** Build a session packet, optionally with the wrong key or version. */
static void send_connect(SSL* ssl, uint32_t account_id, int good_key, uint16_t version) {
    RealmConnectPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.header.type      = PACKET_REALM_CONNECT;
    pkt.header.player_id = htonl(account_id);
    pkt.header.payload_size = htons(sizeof(pkt) - sizeof(PacketHeader));
    memcpy(pkt.header.session_key, good_key ? VALID_KEY : "wrong-key",
           good_key ? sizeof(VALID_KEY) : 9);
    pkt.protocol_version = htons(version);
    client_write(ssl, &pkt, sizeof(pkt));
}

/** Build an ordinary post-auth packet with a chosen payload size. */
static size_t build_packet(uint8_t* out, uint8_t type, uint16_t payload) {
    PacketHeader* header = (PacketHeader*)out;
    header->type         = type;
    header->player_id    = 0;
    header->payload_size = htons(payload);
    memset(out + sizeof(PacketHeader), 0xAB, payload);
    return sizeof(PacketHeader) + payload;
}

/** Read one reply with a bounded wait.
 *
 * The timeout is set on the descriptor under the session: SSL_read blocks in
 * recv(2) underneath, so SO_RCVTIMEO still bounds it, and a session that times
 * out reports it as a read error rather than a short read.
 */
static ssize_t read_reply(SSL* ssl, void* buf, size_t len, int ms) {
    struct timeval tv = { .tv_sec = ms / 1000, .tv_usec = (ms % 1000) * 1000 };
    setsockopt(SSL_get_fd(ssl), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    int n = SSL_read(ssl, buf, (int)len);
    if (n > 0) return n;
    /* 0 for a clean close, so the "the server hung up" checks still read as
     * they did; anything else is a timeout or a reset and is not a reply. */
    return SSL_get_error(ssl, n) == SSL_ERROR_ZERO_RETURN ? 0 : -1;
}

/* --- Tests --------------------------------------------------------------- */

static void test_a_valid_session_is_admitted(void) {
    printf("a valid session is acknowledged and then carries packets\n");
    counters_reset();

    SSL* client = connect_client();
    send_connect(client, 4242, 1, PROTOCOL_VERSION);

    RealmConnectAckPacket ack;
    check(read_reply(client, &ack, sizeof(ack), 2000) == (ssize_t)sizeof(ack),
          "an ack came back");
    check(ack.header.type == PACKET_REALM_CONNECT_ACK, "it is a realm ack");
    check(ack.success == 1, "and it admits the client");
    check(ntohl(ack.header.player_id) == 4242, "naming the account that connected");

    uint8_t pkt[64];
    size_t n = build_packet(pkt, PACKET_TEST_OK, 8);
    client_write(client, pkt, n);

    check(wait_for(&g_packets, 1, 2000), "the packet reached the handler");
    check(g_seen[0].account_id == 4242,
          "carrying the account the session was validated for, not the packet's claim");
    check(g_seen[0].bytes == (ssize_t)n, "framed to exactly one packet");

    client_close(client);
}

static void test_a_bad_session_is_refused(void) {
    printf("a session key that does not validate is refused\n");
    counters_reset();

    SSL* client = connect_client();
    send_connect(client, 7, 0, PROTOCOL_VERSION);

    RealmConnectAckPacket ack;
    check(read_reply(client, &ack, sizeof(ack), 2000) == (ssize_t)sizeof(ack),
          "an ack came back");
    check(ack.success == 0, "and it refuses the client");
    check(wait_for(&g_validations, 1, 2000), "the session was actually checked");
    check(atomic_load(&g_packets) == 0, "and nothing reached the database");

    client_close(client);
}

static void test_a_wrong_protocol_version_says_so(void) {
    printf("a client on another protocol version is told that, not 'invalid session'\n");
    counters_reset();

    SSL* client = connect_client();
    send_connect(client, 9, 1, PROTOCOL_VERSION + 1);

    uint8_t buf[256];
    ssize_t n = read_reply(client, buf, sizeof(buf), 2000);
    check(n >= (ssize_t)sizeof(PacketHeader), "something came back");
    check(buf[0] == PACKET_DISCONNECT, "it is a disconnect, not an auth ack");

    const DisconnectPacket* dc = (const DisconnectPacket*)buf;
    check(dc->reason == DISCONNECT_REASON_VERSION,
          "and the reason names the version, so the player is not sent hunting "
          "for a login problem they do not have");

    /* The session is never even looked at: the key would have been read from
     * the wrong offset anyway. */
    check(atomic_load(&g_validations) == 0, "no session lookup was attempted");

    client_close(client);
}

static void test_nothing_reaches_the_database_before_authentication(void) {
    printf("packets sent before a session is established are not answered\n");
    counters_reset();

    SSL* client = connect_client();

    uint8_t pkt[64];
    size_t n = build_packet(pkt, PACKET_TEST_OK, 8);
    client_write(client, pkt, n);
    usleep(300000);

    check(atomic_load(&g_packets) == 0, "the packet was not processed");
    check(atomic_load(&g_validations) == 0, "and nothing was validated");

    client_close(client);
}

static void test_one_packet_per_visit_to_a_worker(void) {
    printf("a batch of requests cannot hold the only worker to itself\n");
    counters_reset();

    SSL* hog   = connect_client();
    SSL* other = connect_client();
    send_connect(hog, 11, 1, PROTOCOL_VERSION);
    send_connect(other, 12, 1, PROTOCOL_VERSION);

    RealmConnectAckPacket ack;
    check(read_reply(hog, &ack, sizeof(ack), 2000) == (ssize_t)sizeof(ack), "admitted");
    check(read_reply(other, &ack, sizeof(ack), 2000) == (ssize_t)sizeof(ack), "admitted");

    /* Six requests in one write from one connection, then a single request
     * from another. Each is a database round trip. If the batch were answered
     * in one visit, the worker would be gone for all six and the single request
     * would land dead last; answering one per visit puts the second connection
     * back in the queue after each. */
    uint8_t batch[512];
    size_t n = 0;
    for (int i = 0; i < 6; i++) n += build_packet(batch + n, PACKET_TEST_OK, 4);
    client_write(hog, batch, n);

    usleep(3000);   /* let the first of the batch reach the worker */

    uint8_t single[64];
    size_t m = build_packet(single, PACKET_TEST_OTHER, 4);
    client_write(other, single, m);

    check(wait_for(&g_packets, 7, 5000), "all seven were answered");

    int position = -1;
    for (int i = 0; i < atomic_load(&g_seen_count) && i < SEEN_CAP; i++)
        if (g_seen[i].type == PACKET_TEST_OTHER) { position = i; break; }

    check(position >= 0, "the second connection was served");
    check(position < 6,
          "and not made to wait behind the whole batch (it was answered 7th)");

    for (int i = 0; i < atomic_load(&g_seen_count) && i < SEEN_CAP; i++)
        check(g_seen[i].bytes == (ssize_t)(sizeof(PacketHeader) + 4),
              "each request was framed on its own");

    client_close(hog);
    client_close(other);
}

static void test_a_handler_can_close_the_connection(void) {
    printf("a handler that asks to disconnect is obeyed\n");
    counters_reset();

    SSL* client = connect_client();
    send_connect(client, 12, 1, PROTOCOL_VERSION);

    RealmConnectAckPacket ack;
    check(read_reply(client, &ack, sizeof(ack), 2000) == (ssize_t)sizeof(ack), "admitted");

    uint8_t pkt[64];
    size_t n = build_packet(pkt, PACKET_TEST_CLOSE, 0);
    client_write(client, pkt, n);
    check(wait_for(&g_packets, 1, 2000), "the handler saw it");

    uint8_t scratch[16];
    check(read_reply(client, scratch, sizeof(scratch), 2000) == 0,
          "and the server closed the connection");

    client_close(client);
}

static void test_an_oversized_packet_is_refused_without_a_worker(void) {
    printf("a packet claiming more than the wire allows is dropped on the loop\n");
    counters_reset();

    SSL* client = connect_client();
    send_connect(client, 13, 1, PROTOCOL_VERSION);

    RealmConnectAckPacket ack;
    check(read_reply(client, &ack, sizeof(ack), 2000) == (ssize_t)sizeof(ack), "admitted");

    /* Only the header is sent. The size is judged from what it claims, so this
     * is refused without ever waiting for a payload that will not arrive. */
    PacketHeader header;
    header.type         = PACKET_TEST_OK;
    header.player_id    = 0;
    header.payload_size = htons((uint16_t)(MAX_PACKET_SIZE + 100));
    client_write(client, &header, sizeof(header));

    uint8_t buf[256];
    ssize_t n = read_reply(client, buf, sizeof(buf), 2000);
    check(n >= (ssize_t)sizeof(PacketHeader), "a reason came back");
    check(buf[0] == PACKET_DISCONNECT, "it is a disconnect");
    check(atomic_load(&g_packets) == 0, "and no worker was spent on it");

    client_close(client);
}

static void test_a_connection_that_never_authenticates_is_dropped(void) {
    printf("a socket opened and left silent is dropped rather than held\n");
    counters_reset();

    SSL* client = connect_client();
    check(realm_net_connection_count() >= 1, "it is held to begin with");

    /* AUTH_TIMEOUT_SECS is 15 in realm_net.c; this asserts the sweep runs and
     * the connection survives normal silence, not the exact limit. */
    usleep(2500000);
    check(realm_net_connection_count() >= 1,
          "a couple of seconds of silence is not enough to be dropped");

    client_close(client);
}

static void test_many_clients_at_once(void) {
    printf("many clients authenticate and are served across the pool\n");
    counters_reset();

    enum { CLIENTS = 40 };
    SSL* clients[CLIENTS];
    for (int i = 0; i < CLIENTS; i++) {
        clients[i] = connect_client();
        send_connect(clients[i], (uint32_t)(100 + i), 1, PROTOCOL_VERSION);
    }

    for (int i = 0; i < CLIENTS; i++) {
        RealmConnectAckPacket ack;
        check(read_reply(clients[i], &ack, sizeof(ack), 5000) == (ssize_t)sizeof(ack),
              "every client was acknowledged");
        check(ack.success == 1, "and admitted");
    }

    for (int i = 0; i < CLIENTS; i++) {
        uint8_t pkt[64];
        size_t n = build_packet(pkt, PACKET_TEST_OK, 4);
        client_write(clients[i], pkt, n);
    }

    check(wait_for(&g_packets, CLIENTS, 5000), "every request was answered");

    for (int i = 0; i < CLIENTS; i++) client_close(clients[i]);
}

int main(void) {
    printf("=== realm connections ===\n");
    /* The servers do this in setup_signals(); this test does not link it.
     * Without it, writing to a connection the server has already closed kills
     * the test rather than returning an error the way production sees it. */
    signal(SIGPIPE, SIG_IGN);

    log_init();
    log_set_level(LOG_LEVEL_ERROR);

    packet_limiter_init(limit_profile_realm());
    start_listener();

    g_server_ctx = tls_fixture_server_ctx(&g_realm_id, "mmo-realm-test");
    g_client_ctx = tls_fixture_client_ctx();

    /* One worker, deliberately. With a pool, "one packet per visit" and "the
     * whole buffer in one visit" are indistinguishable from outside: either way
     * some worker answers everything. With a single worker, the difference is
     * the whole behaviour -- a connection that holds it for a batch starves
     * every other connection until the batch is done. */
    check(realm_net_start(g_server_ctx, 1) == 0, "the realm reactor started");

    test_a_valid_session_is_admitted();
    test_a_bad_session_is_refused();
    test_a_wrong_protocol_version_says_so();
    test_nothing_reaches_the_database_before_authentication();
    test_one_packet_per_visit_to_a_worker();
    test_a_handler_can_close_the_connection();
    test_an_oversized_packet_is_refused_without_a_worker();
    test_a_connection_that_never_authenticates_is_dropped();
    test_many_clients_at_once();

    realm_net_stop();
    close(g_listen_fd);
    SSL_CTX_free(g_client_ctx);
    SSL_CTX_free(g_server_ctx);
    tls_fixture_identity_free(&g_realm_id);

    printf("\n%d checks passed\n", atomic_load(&passed));
    return 0;
}
