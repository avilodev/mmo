/**
 * @file
 * Check that connecting never blocks the caller, and that recovery backs off.
 *
 * The defect these cover is not a wrong answer, it is a slow one. Connecting
 * used to be a blocking connect() followed by up to ten seconds of Sleep(10)
 * polling, called from inside the frame loop on the render thread, and retried
 * every five seconds. With the realm down, the window stopped answering the
 * operating system for the whole of that, repeatedly.
 *
 * So what is asserted here is wall-clock: begin_* and poll() must return in
 * milliseconds, against an address that will never answer, every time.
 *
 * The realm cases run against a real TLS listener, because the realm link is
 * TLS: the handshake is now one more thing the state machine has to drive
 * across frames without blocking, and it is by far the longest of them. A
 * fixture that spoke plaintext would exercise a path the client no longer
 * takes. The certificate is minted for the run and the pin file written beside
 * it, so the pin check is the real one -- which is what the last case here
 * turns on.
 */

#include "net_internal.h"
#include "net_connect.h"
#include "net_reconnect.h"
#include "cert_spki.h"

#include <assert.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int g_failures = 0;

#define CHECK(cond, what)                                                   \
    do {                                                                    \
        if (cond) {                                                         \
            printf("  ok   %s\n", (what));                                  \
        } else {                                                            \
            printf("  FAIL %s  (%s:%d)\n", (what), __FILE__, __LINE__);     \
            g_failures++;                                                   \
        }                                                                   \
    } while (0)

/** A quieter check, for a loop that runs ten thousand times. */
#define CHECK_SILENT(cond)                                                  \
    do { if (!(cond)) { printf("  FAIL idle poll (%s:%d)\n",                 \
                               __FILE__, __LINE__); g_failures++; } } while (0)

/* --- The state the network layer expects to find ------------------------- */

/* network.c defines the context and the clock, so this test takes only the
 * game-side stubs. */
#define NET_STUBS_LINK_NETWORK_C
#include "hostcompat/net_stubs.c"

static GameState g_game;

/** A monotonic clock in seconds, matching what net_now() reports. */
static double seconds_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* An address in TEST-NET-1 (RFC 5737). Nothing routes it, so a connect there
 * hangs until it times out -- which is exactly the case that used to freeze
 * the window, and the case a non-blocking connect must return from at once. */
#define BLACKHOLE_IP   "192.0.2.1"
#define BLACKHOLE_PORT 7777

/* Loopback on a port nothing listens on: the connect is refused immediately,
 * which is the other shape of failure. */
#define REFUSED_IP   "127.0.0.1"
#define REFUSED_PORT 9

static const char  k_session_key[32] = { 0 };
static const char  k_ticket[64]      = { 0 };

/** Begin a realm connect and time how long the call itself takes. */
static double time_begin_realm(const char* ip, uint16_t port, int* out_started) {
    double t0 = seconds_now();
    int started = network_begin_realm_connect(ip, port, k_session_key, 1234);
    double elapsed = seconds_now() - t0;
    if (out_started) *out_started = started;
    return elapsed;
}

/** Starting a connection must return immediately, whatever the address does. */
static void test_begin_does_not_block(void) {
    printf("beginning a connection returns at once\n");

    int started = 0;
    double elapsed = time_begin_realm(BLACKHOLE_IP, BLACKHOLE_PORT, &started);
    CHECK(started == 1, "an unroutable address still starts an attempt");
    CHECK(elapsed < 0.25,
          "and starting it took under 250ms rather than a SYN timeout");
    if (elapsed >= 0.25)
        printf("       (took %.3fs)\n", elapsed);

    network_connect_abort();

    elapsed = time_begin_realm(REFUSED_IP, REFUSED_PORT, &started);
    CHECK(elapsed < 0.25, "a refused address also returns immediately");
    network_connect_abort();
}

/** Polling must return immediately, however many times it is called. */
static void test_poll_does_not_block(void) {
    printf("polling returns at once\n");

    CHECK(network_begin_realm_connect(BLACKHOLE_IP, BLACKHOLE_PORT,
                                      k_session_key, 1234) == 1,
          "an attempt is in flight");

    double worst = 0.0;
    int    polls = 0;
    for (int i = 0; i < 200; i++) {
        double t0 = seconds_now();
        NetConnectPhase p = network_connect_poll();
        double took = seconds_now() - t0;
        if (took > worst) worst = took;
        polls++;
        if (p != NET_CONNECT_PENDING) break;
    }

    CHECK(worst < 0.05, "no single poll took as long as 50ms");
    if (worst >= 0.05) printf("       (worst poll %.3fs over %d polls)\n", worst, polls);

    CHECK(network_connect_in_flight() == 1,
          "and the attempt is still pending, not silently abandoned");
    network_connect_abort();
}

/** A refused connection is reported as a failure, not left pending forever. */
static void test_refused_connection_reports_failure(void) {
    printf("a refused connection fails\n");

    CHECK(network_begin_world_connect(REFUSED_IP, REFUSED_PORT, k_ticket, 99) == 1,
          "the attempt started");

    NetConnectPhase phase = NET_CONNECT_PENDING;
    double deadline = seconds_now() + 3.0;
    while (phase == NET_CONNECT_PENDING && seconds_now() < deadline) {
        phase = network_connect_poll();
    }

    CHECK(phase == NET_CONNECT_FAILED, "it was reported as failed");
    CHECK(network_connect_in_flight() == 0, "and nothing is left in flight");
    CHECK(network_connect_message()[0] != '\0', "with a message for the status line");
    printf("       reason: %s\n", network_connect_message());
}

/** A result is reported exactly once, then the machine is idle. */
static void test_result_is_reported_once(void) {
    printf("a result is reported once\n");

    network_begin_world_connect(REFUSED_IP, REFUSED_PORT, k_ticket, 99);

    NetConnectPhase phase = NET_CONNECT_PENDING;
    double deadline = seconds_now() + 3.0;
    while (phase == NET_CONNECT_PENDING && seconds_now() < deadline)
        phase = network_connect_poll();

    CHECK(phase == NET_CONNECT_FAILED, "the failure was reported");
    CHECK(network_connect_poll() == NET_CONNECT_IDLE,
          "the next poll reports idle rather than the same failure again");
    CHECK(network_connect_poll() == NET_CONNECT_IDLE, "and stays idle");
}

/** Polling with nothing in flight is idle and free. */
static void test_idle_poll(void) {
    printf("idle polling\n");
    network_connect_abort();

    double t0 = seconds_now();
    for (int i = 0; i < 10000; i++)
        CHECK_SILENT(network_connect_poll() == NET_CONNECT_IDLE);
    double elapsed = seconds_now() - t0;

    CHECK(elapsed < 0.1, "10,000 idle polls cost under 100ms");
}

/** Recovery backs off, and gives up rather than looping forever. */
static void test_reconnect_backoff(void) {
    printf("recovery backs off and eventually gives up\n");

    /* A refused address rather than an unroutable one: each attempt then fails
     * in milliseconds of real time, so the eight of them fit in a test while
     * the backoff between them is driven on the synthetic clock below. */
    net_reconnect_remember_world(REFUSED_IP, REFUSED_PORT, 1234,
                                 "00112233445566778899aabbccddeeff"
                                 "00112233445566778899aabbccddeeff",
                                 3, 77);
    net_reconnect_begin(0.0);

    CHECK(net_reconnect_phase() == RECONNECT_WAITING,
          "the first thing recovery does is wait, not hammer");

    /* Drive it on a synthetic clock so the test does not have to spend the
     * backoff in real time. */
    double clock = 0.0;
    double last_wait = 0.0;
    int    waits_seen = 0;
    double waits[16];

    for (int step = 0; step < 200000 && net_reconnect_phase() != RECONNECT_GIVEN_UP; step++) {
        if (net_reconnect_phase() == RECONNECT_WAITING) {
            double remaining = net_reconnect_seconds_until_retry(clock);
            if (remaining > 0.0) {
                if (remaining != last_wait && waits_seen < 16) {
                    waits[waits_seen++] = remaining;
                    last_wait = remaining;
                }
                clock += remaining;   /* jump to the moment the retry is due */
            }
        }
        clock += 0.016;               /* one frame */
        net_reconnect_update(clock);
    }

    CHECK(net_reconnect_phase() == RECONNECT_GIVEN_UP,
          "recovery gives up rather than retrying forever");
    CHECK(waits_seen >= 3, "and it waited between attempts");

    int growing = 1;
    for (int i = 1; i < waits_seen && i < 5; i++)
        if (waits[i] <= waits[i - 1]) growing = 0;
    CHECK(growing, "each wait was longer than the one before it");

    printf("       waits: ");
    for (int i = 0; i < waits_seen && i < 6; i++) printf("%.2fs ", waits[i]);
    printf("\n");
}

/** With nothing remembered, recovery says so instead of pretending to try. */
static void test_reconnect_without_a_session(void) {
    printf("recovery with no remembered session\n");

    net_reconnect_forget();
    net_reconnect_begin(0.0);

    CHECK(net_reconnect_phase() == RECONNECT_GIVEN_UP,
          "it gives up immediately rather than looping on nothing");
    CHECK(strstr(net_reconnect_status(), "restart") != NULL,
          "and tells the player what to do");
}

/** Cancelling resets the machine so a later drop starts fresh. */
static void test_reconnect_cancel(void) {
    printf("cancelling recovery\n");

    net_reconnect_remember_world(BLACKHOLE_IP, BLACKHOLE_PORT, 1234,
                                 "00112233445566778899aabbccddeeff"
                                 "00112233445566778899aabbccddeeff",
                                 3, 77);
    net_reconnect_begin(0.0);
    CHECK(net_reconnect_phase() != RECONNECT_IDLE, "recovery is under way");

    net_reconnect_cancel();
    CHECK(net_reconnect_phase() == RECONNECT_IDLE, "cancelling returns it to idle");
    CHECK(net_reconnect_attempts() == 0, "and resets the attempt count");
    CHECK(network_connect_in_flight() == 0, "and abandons any socket in flight");
}


/* --- A realm that actually speaks TLS -------------------------------------
 *
 * Minted here rather than read from disk: a test that needed a certificate
 * checked in beside it would either skip itself when the file was missing or
 * carry a key pair in the repository, and the first of those is how the login
 * server's own transport test came to report success by not running.
 */

#define PIN_FILE "/tmp/mmo_client_realm_pins.txt"

static EVP_PKEY*  g_realm_key   = NULL;
static X509*      g_realm_cert  = NULL;
static EVP_PKEY*  g_other_key   = NULL;   /* a key the client does not pin */
static X509*      g_other_cert  = NULL;
static SSL_CTX*   g_realm_ctx   = NULL;
static SSL_CTX*   g_other_ctx   = NULL;

/** Generate one self-signed identity. */
static void make_identity(EVP_PKEY** key, X509** cert, const char* cn) {
    *key = EVP_RSA_gen(2048);
    assert(*key);
    *cert = X509_new();
    assert(*cert);

    X509_set_version(*cert, 2);
    ASN1_INTEGER_set(X509_get_serialNumber(*cert), 1);
    X509_gmtime_adj(X509_getm_notBefore(*cert), 0);
    X509_gmtime_adj(X509_getm_notAfter(*cert), 3600);
    X509_set_pubkey(*cert, *key);

    X509_NAME* name = X509_get_subject_name(*cert);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                               (const unsigned char*)cn, -1, -1, 0);
    X509_set_issuer_name(*cert, name);
    assert(X509_sign(*cert, *key, EVP_sha256()) > 0);
}

/** Build a server context presenting one identity. */
static SSL_CTX* server_ctx_for(X509* cert, EVP_PKEY* key) {
    SSL_CTX* ctx = SSL_CTX_new(TLS_server_method());
    assert(ctx);
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    assert(SSL_CTX_use_certificate(ctx, cert) == 1);
    assert(SSL_CTX_use_PrivateKey(ctx, key) == 1);
    return ctx;
}

/** Write the pin file the client will load, naming one certificate. */
static void write_pin_file(X509* cert) {
    /* Through cert_spki_digest(), the same function the game itself uses. A
     * fixture that computed the pin its own way would agree with a client that
     * computed it the same wrong way, which is exactly how the SPKI/bit-string
     * confusion survived in this tree. */
    unsigned char digest[32];
    assert(cert_spki_digest(cert, digest) == 1);

    FILE* f = fopen(PIN_FILE, "w");
    assert(f);
    fprintf(f, "# minted by net_connect_test\n");
    for (unsigned i = 0; i < 32; i++) fprintf(f, "%02x", digest[i]);
    fprintf(f, "\n");
    fclose(f);
}

static void tls_fixture_setup(void) {
    make_identity(&g_realm_key, &g_realm_cert, "mmo-realm-test");
    make_identity(&g_other_key, &g_other_cert, "mmo-not-the-realm");
    g_realm_ctx = server_ctx_for(g_realm_cert, g_realm_key);
    g_other_ctx = server_ctx_for(g_other_cert, g_other_key);

    /* Only the realm's key goes into the pin file. The other exists to be
     * refused. */
    write_pin_file(g_realm_cert);
    setenv("MMO_REALM_PINS", PIN_FILE, 1);
}

static void tls_fixture_teardown(void) {
    SSL_CTX_free(g_other_ctx);
    SSL_CTX_free(g_realm_ctx);
    X509_free(g_other_cert);
    EVP_PKEY_free(g_other_key);
    X509_free(g_realm_cert);
    EVP_PKEY_free(g_realm_key);
    unlink(PIN_FILE);
}

/** What the server thread was asked to do, and what came of it. */
typedef struct {
    int      listen_fd;
    SSL_CTX* ctx;
    int      fd;        /**< The accepted descriptor. */
    SSL*     ssl;       /**< NULL when the handshake did not complete. */
} RealmSide;

/** Accept one connection and complete a handshake, on its own thread.
 *
 * A thread because both halves block on each other: SSL_accept waits for the
 * client, and the client's half is driven by network_connect_poll() on this
 * one. Everything the state machine does stays non-blocking; it is the
 * *fixture* that blocks, which is the opposite of the arrangement these tests
 * exist to prevent.
 */
static void* realm_thread(void* arg) {
    RealmSide* r = arg;
    r->fd = (int)accept(r->listen_fd, NULL, NULL);
    if (r->fd < 0) return NULL;

    SSL* ssl = SSL_new(r->ctx);
    SSL_set_fd(ssl, r->fd);
    if (SSL_accept(ssl) == 1) r->ssl = ssl;
    else                      SSL_free(ssl);
    return NULL;
}

/* --- The handshake window ------------------------------------------------
 *
 * A real loopback listener, because what is under test is what the read path
 * does with bytes that arrive between the connect packet and its answer. The
 * blackhole and refused addresses above never get that far.
 */

/** Listen on an ephemeral loopback port. @return the listening socket, or -1. */
static int open_listener(uint16_t* out_port) {
    int ls = (int)socket(AF_INET, SOCK_STREAM, 0);
    if (ls < 0) return -1;

    int on = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (const char*)&on, sizeof(on));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port        = 0;                      /* let the kernel choose */

    if (bind(ls, (struct sockaddr*)&addr, sizeof(addr)) < 0 || listen(ls, 1) < 0) {
        closesocket(ls);
        return -1;
    }

    socklen_t len = sizeof(addr);
    if (getsockname(ls, (struct sockaddr*)&addr, &len) < 0) {
        closesocket(ls);
        return -1;
    }
    *out_port = ntohs(addr.sin_port);
    return ls;
}

/** Poll the connect state machine until it leaves PENDING or the deadline passes. */
static NetConnectPhase poll_until_settled(double timeout_seconds) {
    double deadline = seconds_now() + timeout_seconds;
    NetConnectPhase r = NET_CONNECT_PENDING;
    while (seconds_now() < deadline) {
        r = network_connect_poll();
        if (r != NET_CONNECT_PENDING) break;
    }
    return r;
}

/**
 * Traffic arriving before the acknowledgement is not acted on.
 *
 * The socket is marked connected before the ack arrives, because the ack can
 * only be read through the path that flag gates. What that used to mean is
 * that anything else the server sent in that window was dispatched into a
 * session that did not exist yet.
 *
 * A FORM_SWAP_ACK stands in for all of it: it is an ordinary gameplay reply
 * with a visible landing site (`g_net.form_swap.ready`), and it has no business
 * being believed by a client whose connect packet has not been answered.
 */
static void test_traffic_before_the_ack_is_ignored(void) {
    printf("\ntraffic arriving before the connect ack\n");

    uint16_t port = 0;
    int ls = open_listener(&port);
    CHECK(ls >= 0, "a loopback listener came up");
    if (ls < 0) return;

    RealmSide realm = {.listen_fd = ls, .ctx = g_realm_ctx, .fd = -1, .ssl = NULL};
    pthread_t t;
    pthread_create(&t, NULL, realm_thread, &realm);

    int started = network_begin_realm_connect("127.0.0.1", port, k_session_key, 1234);
    CHECK(started, "the connect attempt began");

    /* Drive the state machine to the point where it has completed the TLS
     * handshake, sent its connect packet, and is waiting: that is the window
     * under test. */
    double deadline = seconds_now() + 5.0;
    while (!g_net.handshaking && seconds_now() < deadline) network_connect_poll();
    pthread_join(t, NULL);

    CHECK(realm.ssl != NULL, "the realm completed a TLS handshake with the client");
    CHECK(g_net.handshaking, "the client is waiting for its acknowledgement");
    CHECK(g_net.connected, "and the socket is marked connected while it waits");
    if (!realm.ssl) { closesocket(ls); return; }

    /* The connect packet arrived encrypted, and carries the session key. Read
     * it back to prove the link is actually carrying protocol -- a handshake
     * that completed but delivered nothing would pass every check above. */
    RealmConnectPacket got;
    memset(&got, 0, sizeof(got));
    int n = SSL_read(realm.ssl, &got, sizeof(got));
    CHECK(n == (int)sizeof(got), "the connect packet arrived over the session");
    CHECK(got.header.type == PACKET_REALM_CONNECT, "and it is a realm connect");
    CHECK(memcmp(got.header.session_key, k_session_key, 32) == 0,
          "carrying the session key, which never went out in the clear");

    /* A gameplay reply, sent before the handshake has been answered. */
    g_net.form_swap.ready = FALSE;
    FormSwapAckPacket swap;
    memset(&swap, 0, sizeof(swap));
    swap.header.type         = PACKET_FORM_SWAP_ACK;
    swap.header.payload_size = htons(sizeof(swap) - sizeof(PacketHeader));
    SSL_write(realm.ssl, &swap, sizeof(swap));

    /* One poll is enough to read and route it; several, to be sure. */
    for (int i = 0; i < 20; i++) network_connect_poll();
    CHECK(!g_net.form_swap.ready,
          "a gameplay packet sent during the handshake was not acted on");

    /* Now answer the handshake, and the same opcode must be believed. */
    RealmConnectAckPacket ack;
    memset(&ack, 0, sizeof(ack));
    ack.header.type         = PACKET_REALM_CONNECT_ACK;
    ack.header.payload_size = htons(sizeof(ack) - sizeof(PacketHeader));
    ack.success             = 1;
    snprintf(ack.message, sizeof(ack.message), "welcome");
    SSL_write(realm.ssl, &ack, sizeof(ack));

    NetConnectPhase r = poll_until_settled(2.0);
    CHECK(r == NET_CONNECT_SUCCEEDED, "the acknowledgement completed the handshake");
    CHECK(!g_net.handshaking, "and the handshake window closed");

    SSL_write(realm.ssl, &swap, sizeof(swap));
    for (int i = 0; i < 20; i++) network_update();
    CHECK(g_net.form_swap.ready,
          "the same packet after the acknowledgement IS acted on");

    network_connect_abort();
    SSL_free(realm.ssl);
    closesocket(realm.fd);
    closesocket(ls);
}

/**
 * A realm whose key is not pinned is refused, and no session key is sent.
 *
 * This is the property the whole change exists for. Encrypting the realm link
 * without checking who is on the other end of it would move the session key
 * from "readable by anyone on the path" to "readable by anyone on the path who
 * can also answer on the port", which is not much of a move -- and none of the
 * cases above would notice, because a handshake against an impostor completes
 * exactly like a handshake against the realm.
 *
 * So the assertion is not only that the attempt fails: it is that nothing was
 * written after the handshake. The client must decide who it is talking to
 * before its first byte of protocol, not after.
 */
static void test_an_unpinned_realm_is_refused(void) {
    printf("\na realm whose key is not pinned\n");

    uint16_t port = 0;
    int ls = open_listener(&port);
    CHECK(ls >= 0, "a loopback listener came up");
    if (ls < 0) return;

    /* g_other_ctx presents a perfectly valid certificate for a key the client
     * has never been told about -- which is what an attacker who can answer on
     * this port actually holds. */
    RealmSide realm = {.listen_fd = ls, .ctx = g_other_ctx, .fd = -1, .ssl = NULL};
    pthread_t t;
    pthread_create(&t, NULL, realm_thread, &realm);

    int started = network_begin_realm_connect("127.0.0.1", port, k_session_key, 1234);
    CHECK(started, "the connect attempt began");

    NetConnectPhase r = poll_until_settled(5.0);
    pthread_join(t, NULL);

    CHECK(r == NET_CONNECT_FAILED, "the attempt failed");
    CHECK(!g_net.connected, "the client is not connected");
    CHECK(!g_net.handshaking, "and never entered the handshake window");

    /* The impostor's own handshake succeeded -- it is the client that refused
     * afterwards -- so there is a session to read from, and it must be empty. */
    if (realm.ssl) {
        struct timeval tv = {.tv_sec = 0, .tv_usec = 200000};
        setsockopt(realm.fd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
        uint8_t scratch[256];
        int n = SSL_read(realm.ssl, scratch, sizeof(scratch));
        CHECK(n <= 0, "and sent it nothing at all — no session key reached it");
        SSL_free(realm.ssl);
    }
    if (realm.fd >= 0) closesocket(realm.fd);
    closesocket(ls);
}

/**
 * The session payload arriving in the same read as the ack is acted on.
 *
 * The companion to the case above, and the one that was wrong. A server does
 * not send an acknowledgement and then wait to be asked: the world queues the
 * ack, the character record, the stats, the ability bar and the quest log into
 * one outbound buffer and they arrive together. The gate that refuses
 * pre-session traffic was cleared by finish_succeeded(), which runs in
 * network_connect_poll() *after* network_update() has already drained the whole
 * buffer -- so everything behind the ack in that same read was dropped.
 *
 * Character data and stats survived it, because the client re-requests them
 * once it is in the world. PACKET_ABILITY_DATA and the quest packets have no
 * request opcode at all: they are sent once, here, and a player who lost them
 * played the session with an empty ability bar and an empty quest log.
 *
 * The check is that ONE write carrying both is fully acted on. Writing them
 * separately, as the case above does, passes either way.
 */
static void test_payload_behind_the_ack_is_delivered(void) {
    printf("\nthe session payload sent with the ack\n");

    uint16_t port = 0;
    int ls = open_listener(&port);
    CHECK(ls >= 0, "a loopback listener came up");
    if (ls < 0) return;

    RealmSide realm = {.listen_fd = ls, .ctx = g_realm_ctx, .fd = -1, .ssl = NULL};
    pthread_t t;
    pthread_create(&t, NULL, realm_thread, &realm);

    int started = network_begin_realm_connect("127.0.0.1", port, k_session_key, 1234);
    CHECK(started, "the connect attempt began");

    double deadline = seconds_now() + 5.0;
    while (!g_net.handshaking && seconds_now() < deadline) network_connect_poll();
    pthread_join(t, NULL);

    CHECK(realm.ssl != NULL, "the realm completed a TLS handshake with the client");
    if (!realm.ssl) { closesocket(ls); return; }

    RealmConnectPacket got;
    memset(&got, 0, sizeof(got));
    SSL_read(realm.ssl, &got, sizeof(got));
    CHECK(got.header.type == PACKET_REALM_CONNECT, "the connect packet arrived");

    /* One buffer, one write: the ack and a gameplay packet behind it. */
    uint8_t burst[sizeof(RealmConnectAckPacket) + sizeof(FormSwapAckPacket)];
    memset(burst, 0, sizeof(burst));

    RealmConnectAckPacket* ack = (RealmConnectAckPacket*)burst;
    ack->header.type         = PACKET_REALM_CONNECT_ACK;
    ack->header.payload_size = htons(sizeof(*ack) - sizeof(PacketHeader));
    ack->success             = 1;
    snprintf(ack->message, sizeof(ack->message), "welcome");

    FormSwapAckPacket* swap =
        (FormSwapAckPacket*)(burst + sizeof(RealmConnectAckPacket));
    swap->header.type         = PACKET_FORM_SWAP_ACK;
    swap->header.payload_size = htons(sizeof(*swap) - sizeof(PacketHeader));

    g_net.form_swap.ready = FALSE;
    SSL_write(realm.ssl, burst, sizeof(burst));

    NetConnectPhase r = poll_until_settled(2.0);
    CHECK(r == NET_CONNECT_SUCCEEDED, "the acknowledgement completed the handshake");
    CHECK(!g_net.handshaking, "and the handshake window closed");
    CHECK(g_net.form_swap.ready,
          "the packet behind the ack, in the same read, was acted on");

    network_connect_abort();
    SSL_free(realm.ssl);
    closesocket(realm.fd);
    closesocket(ls);
}

/**
 * A refused handshake reports what the server said, not that it hung up.
 *
 * A realm that refuses a connection sends PACKET_DISCONNECT with a reason and
 * a sentence for the player, and then closes. Both arrive in one read: the
 * bytes, then the FIN.
 *
 * network_update() used to drop the link the moment the FIN came back and
 * return from there -- before the dispatch loop, and after drop_link() had
 * already cleared the reassembly buffer. So the one packet whose whole purpose
 * is to explain the disconnect was the one packet guaranteed to be thrown
 * away, and every refusal reached the player as "the server closed the
 * connection during the handshake". A client built against an older
 * PROTOCOL_VERSION was then indistinguishable from a realm that was down: the
 * world list sat on "Loading..." with nothing anywhere saying to rebuild.
 */
static void test_a_refusal_reports_the_servers_reason(void) {
    printf("\na realm that refuses the handshake\n");

    network_connect_abort();
    network_clear_disconnect_reason();

    uint16_t port = 0;
    int ls = open_listener(&port);
    CHECK(ls >= 0, "a loopback listener came up");
    if (ls < 0) return;

    RealmSide realm = {.listen_fd = ls, .ctx = g_realm_ctx, .fd = -1, .ssl = NULL};
    pthread_t t;
    pthread_create(&t, NULL, realm_thread, &realm);

    int started = network_begin_realm_connect("127.0.0.1", port, k_session_key, 1234);
    CHECK(started, "the connect attempt began");

    double deadline = seconds_now() + 5.0;
    while (!g_net.handshaking && seconds_now() < deadline) network_connect_poll();
    pthread_join(t, NULL);

    CHECK(realm.ssl != NULL, "the realm completed a TLS handshake with the client");
    if (!realm.ssl) { closesocket(ls); return; }

    RealmConnectPacket got;
    memset(&got, 0, sizeof(got));
    SSL_read(realm.ssl, &got, sizeof(got));
    CHECK(got.header.type == PACKET_REALM_CONNECT, "the connect packet arrived");

    /* What realm_net.c sends a client whose PROTOCOL_VERSION it does not
     * share: no acknowledgement, a reason, and then the socket. */
    static const char* const k_said =
        "This client is a different version than the server. Please update.";

    DisconnectPacket bye;
    memset(&bye, 0, sizeof(bye));
    bye.header.type         = PACKET_DISCONNECT;
    bye.header.payload_size = htons(sizeof(bye) - sizeof(PacketHeader));
    bye.reason              = DISCONNECT_REASON_VERSION;
    snprintf(bye.message, sizeof(bye.message), "%s", k_said);

    SSL_write(realm.ssl, &bye, sizeof(bye));
    SSL_shutdown(realm.ssl);
    SSL_free(realm.ssl);
    closesocket(realm.fd);
    realm.ssl = NULL;
    realm.fd  = -1;

    NetConnectPhase r = poll_until_settled(2.0);
    CHECK(r == NET_CONNECT_FAILED, "the attempt failed");

    uint8_t reason = 0;
    char    said[128] = {0};
    CHECK(network_get_disconnect_reason(&reason, said, (int)sizeof(said)),
          "the disconnect packet was dispatched, not discarded with the buffer");
    CHECK(reason == DISCONNECT_REASON_VERSION, "and carried the version reason");
    CHECK(strcmp(said, k_said) == 0, "and the sentence meant for the player");

    const char* reported = network_connect_message();
    CHECK(strstr(reported, "different version") != NULL,
          "the reported failure names the version mismatch");
    CHECK(strstr(reported, "closed the connection") == NULL,
          "and is not the generic \"the server closed the connection\" line");

    network_connect_abort();
    network_clear_disconnect_reason();
    closesocket(ls);
}

/**
 * A world handshake is not reported as a realm handshake.
 *
 * network_connect_poll() reports SUCCEEDED and FAILED exactly once, so only the
 * caller that started an attempt may poll it -- and there are three callers:
 * the frame loop drives the realm, character select drives a world entry, and
 * the reconnect supervisor drives both halves of a recovery.
 *
 * The frame loop used to decide with network_connect_in_flight(), which is
 * equally true of a world handshake somebody else started. Because it runs
 * before game_update(), it took that result every frame: the frame loop marked
 * the client connected to the realm while character select, polling second,
 * saw NET_CONNECT_IDLE and sat in "Entering world..." until it gave up. World
 * entry could not complete at all. This is the test that separates them.
 */
static void test_world_attempt_is_not_a_realm_attempt(void) {
    printf("\ntelling a world handshake from a realm one\n");

    network_connect_abort();
    CHECK(!network_connect_in_flight(), "nothing is in flight to begin with");
    CHECK(!network_realm_connect_in_flight(), "and so no realm attempt either");

    char ticket[64];
    memset(ticket, 0, sizeof(ticket));
    snprintf(ticket, sizeof(ticket), "deadbeef");

    /* The blackhole address: the connect stays pending, which is the state
     * under test. Whether it would ever succeed does not matter here. */
    int started = network_begin_world_connect(BLACKHOLE_IP, 7000, ticket, 42);
    CHECK(started, "a world attempt began");
    CHECK(network_connect_in_flight(), "and is in flight");
    CHECK(!network_realm_connect_in_flight(),
          "but is NOT reported as a realm attempt");

    network_connect_abort();

    started = network_begin_realm_connect(BLACKHOLE_IP, 7000, k_session_key, 1234);
    CHECK(started, "a realm attempt began");
    CHECK(network_connect_in_flight(), "and is in flight");
    CHECK(network_realm_connect_in_flight(), "and IS reported as a realm attempt");

    network_connect_abort();
}

int main(void) {
    printf("=== client connection state machine ===\n");

    /* The fixture writes to sockets the client under test has deliberately
     * closed -- refusing an unpinned realm is exactly that -- and on this host
     * the default for a write to a closed peer is to kill the process. The
     * game itself never sees this: Winsock has no SIGPIPE, and the Linux build
     * exists to compile and test the tree, not to ship. */
    signal(SIGPIPE, SIG_IGN);

    memset(&g_game, 0, sizeof(g_game));
    g_current_game = &g_game;

    tls_fixture_setup();
    assert(network_init(1234));

    test_begin_does_not_block();
    test_poll_does_not_block();
    test_refused_connection_reports_failure();
    test_result_is_reported_once();
    test_idle_poll();
    test_reconnect_backoff();
    test_reconnect_without_a_session();
    test_reconnect_cancel();
    test_traffic_before_the_ack_is_ignored();
    test_payload_behind_the_ack_is_delivered();
    test_a_refusal_reports_the_servers_reason();
    test_world_attempt_is_not_a_realm_attempt();
    test_an_unpinned_realm_is_refused();

    network_connect_abort();
    network_cleanup();
    tls_fixture_teardown();

    if (g_failures) {
        printf("\n%d connection check(s) FAILED\n", g_failures);
        return 1;
    }
    printf("\nAll connection checks passed\n");
    return 0;
}
