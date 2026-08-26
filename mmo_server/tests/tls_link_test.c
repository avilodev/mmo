/**
 * @file
 * Check the mutually authenticated TLS link the realm and worlds talk over.
 *
 * The realm↔world hop carries the shared server auth key -- the credential a
 * realm proves itself with, and the one a world accepts as licence to be listed
 * and to admit the world-entry tickets that realm mints. It used to be
 * plaintext. What replaced it is not "TLS" in general but a specific claim:
 * each end refuses the other unless its public key is one it was told about
 * ahead of time, and it refuses it *before* the server key is written.
 *
 * That ordering is the whole point, so it is what these cases assert. A link
 * that encrypted the key and then handed it to anyone who asked would pass a
 * test that only checked for encryption.
 *
 * The realm↔world handshake in world_server/src/main.c and the connect in
 * realm_server/src/world_connect.c are both thin wrappers over what is tested
 * here: load pins, hand them to tls_accept_pinned/tls_connect_pinned, and only
 * then read or write protocol bytes.
 */

#include "tls.h"
#include "tls_fixture.h"
#include "log.h"

#include <arpa/inet.h>
#include <assert.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static atomic_int passed;

static void check(int condition, const char* what) {
    printf("  %-4s %s\n", condition ? "ok" : "FAIL", what);
    if (!condition) exit(1);
    atomic_fetch_add(&passed, 1);
}

/* --- Identities ---------------------------------------------------------- */

static TlsFixtureIdentity g_world_id;    /**< What a world presents. */
static TlsFixtureIdentity g_realm_id;    /**< What the realm presents. */
static TlsFixtureIdentity g_impostor_id; /**< A key nobody pinned. */

static SSL_CTX* g_world_ctx    = NULL;   /**< World side: demands a certificate. */
static SSL_CTX* g_realm_ctx    = NULL;   /**< Realm side: presents one. */
static SSL_CTX* g_impostor_ctx = NULL;   /**< Client presenting the unpinned key. */
static SSL_CTX* g_impostor_srv = NULL;   /**< Server presenting the same. */

/* Where the fixture's PEM and pin files live for the run. */
#define W_CRT  "/tmp/mmo_tls_world.crt"
#define W_KEY  "/tmp/mmo_tls_world.key"
#define R_CRT  "/tmp/mmo_tls_realm.crt"
#define R_KEY  "/tmp/mmo_tls_realm.key"
#define I_CRT  "/tmp/mmo_tls_impostor.crt"
#define I_KEY  "/tmp/mmo_tls_impostor.key"
#define R_PINS "/tmp/mmo_tls_realm_pins.txt"
#define W_PINS "/tmp/mmo_tls_world_pins.txt"

static CertPinSet g_realm_pins;   /**< Realms a world will accept. */
static CertPinSet g_world_pins;   /**< Worlds a realm will connect to. */

/* --- A connected pair ---------------------------------------------------- */

/** One accepted socket pair: the world's end and the realm's end. */
typedef struct { int world_fd; int realm_fd; } Pair;

static int g_listen_fd = -1;
static int g_listen_port = 0;

static void start_listener(void) {
    g_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(g_listen_fd >= 0);
    int one = 1;
    setsockopt(g_listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    assert(bind(g_listen_fd, (struct sockaddr*)&addr, sizeof(addr)) == 0);
    assert(listen(g_listen_fd, 16) == 0);

    socklen_t len = sizeof(addr);
    assert(getsockname(g_listen_fd, (struct sockaddr*)&addr, &len) == 0);
    g_listen_port = ntohs(addr.sin_port);
}

static Pair connect_pair(void) {
    Pair p;
    p.realm_fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(p.realm_fd >= 0);

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((uint16_t)g_listen_port);
    assert(connect(p.realm_fd, (struct sockaddr*)&addr, sizeof(addr)) == 0);

    p.world_fd = accept(g_listen_fd, NULL, NULL);
    assert(p.world_fd >= 0);
    return p;
}

/** What the world's thread was asked to do, and what came of it. */
typedef struct {
    int         fd;
    SSL_CTX*    ctx;
    CertPinSet* pins;
    SSL*        result;      /**< NULL when the world refused the peer. */
} WorldSide;

/** Run the world's half of a handshake on its own thread.
 *
 * Both halves block, so they cannot run on one thread: SSL_accept and
 * SSL_connect each wait for the other to speak.
 */
static void* world_thread(void* arg) {
    WorldSide* w = arg;
    w->result = tls_accept_pinned(w->ctx, w->fd, w->pins, "realm-under-test", 5000);
    return NULL;
}

/* --- Tests --------------------------------------------------------------- */

static void test_a_pinned_pair_completes(void) {
    printf("a realm and a world that pin each other complete a handshake\n");

    Pair pair = connect_pair();
    WorldSide w = {.fd = pair.world_fd, .ctx = g_world_ctx, .pins = &g_realm_pins};
    pthread_t t;
    pthread_create(&t, NULL, world_thread, &w);

    SSL* realm = tls_connect_pinned(g_realm_ctx, pair.realm_fd, &g_world_pins,
                                    "world-under-test", 5000);
    pthread_join(t, NULL);

    check(realm != NULL, "the realm accepted the world's key");
    check(w.result != NULL, "and the world accepted the realm's");

    /* Round-trip a payload, so "handshake completed" is not the whole claim. */
    if (realm && w.result) {
        const char sent[] = "server-auth-key";
        char got[sizeof(sent)] = {0};
        check(tls_send_exact(realm, sent, sizeof(sent), 2000), "the realm wrote a packet");
        check(tls_recv_exact(w.result, got, sizeof(got), 2000), "the world read it");
        check(memcmp(sent, got, sizeof(sent)) == 0, "and it arrived intact");
    }

    if (realm) tls_close(realm);
    if (w.result) tls_close(w.result);
    close(pair.realm_fd);
    close(pair.world_fd);
}

static void test_two_packets_in_one_record(void) {
    printf("a record carrying two packets yields both without a second poll\n");

    Pair pair = connect_pair();
    WorldSide w = {.fd = pair.world_fd, .ctx = g_world_ctx, .pins = &g_realm_pins};
    pthread_t t;
    pthread_create(&t, NULL, world_thread, &w);
    SSL* realm = tls_connect_pinned(g_realm_ctx, pair.realm_fd, &g_world_pins,
                                    "world-under-test", 5000);
    pthread_join(t, NULL);
    assert(realm && w.result);

    /* One write, two packets. tls_recv_exact() must find the second inside the
     * session rather than polling the descriptor for it -- the descriptor has
     * nothing left to report, so a poll-first reader would block until the
     * deadline and call a live link dead. That is exactly how the world's
     * heartbeat loop used to be written. */
    uint8_t both[64];
    memset(both, 0xA5, sizeof(both));
    assert(tls_send_exact(realm, both, sizeof(both), 2000));

    uint8_t first[32], second[32];
    check(tls_recv_exact(w.result, first, sizeof(first), 2000), "the first packet arrives");
    /* A deliberately short deadline: if this needed the descriptor to become
     * readable again it would time out, because it never will. */
    check(tls_recv_exact(w.result, second, sizeof(second), 300),
          "and the second comes out of the session, not off the socket");
    check(memcmp(first, both, 32) == 0 && memcmp(second, both + 32, 32) == 0,
          "both carry what was written");

    tls_close(realm);
    tls_close(w.result);
    close(pair.realm_fd);
    close(pair.world_fd);
}

static void test_an_unpinned_realm_is_refused(void) {
    printf("a world refuses a realm whose key it does not pin\n");

    Pair pair = connect_pair();
    WorldSide w = {.fd = pair.world_fd, .ctx = g_world_ctx, .pins = &g_realm_pins};
    pthread_t t;
    pthread_create(&t, NULL, world_thread, &w);

    /* The impostor holds a valid certificate for a key nobody pinned -- which
     * is what an attacker who can answer on the port actually has. */
    SSL* impostor = tls_connect_pinned(g_impostor_ctx, pair.realm_fd, &g_world_pins,
                                       "world-under-test", 5000);
    pthread_join(t, NULL);

    check(w.result == NULL, "the world refused it");

    if (impostor) tls_close(impostor);
    close(pair.realm_fd);
    close(pair.world_fd);
}

static void test_an_unpinned_world_is_refused(void) {
    printf("a realm refuses a world whose key it does not pin\n");

    /* The world side presents the impostor key. The realm's pin set names the
     * real world, so the connect must fail even though the handshake itself
     * would succeed. */
    Pair pair = connect_pair();
    WorldSide w = {.fd = pair.world_fd, .ctx = g_impostor_srv, .pins = NULL};
    pthread_t t;
    pthread_create(&t, NULL, world_thread, &w);

    SSL* realm = tls_connect_pinned(g_realm_ctx, pair.realm_fd, &g_world_pins,
                                    "world-under-test", 5000);
    pthread_join(t, NULL);

    check(realm == NULL, "the realm refused it, so no auth key was written");

    if (w.result) tls_close(w.result);
    close(pair.realm_fd);
    close(pair.world_fd);
}

static void test_a_peer_with_no_certificate_is_refused(void) {
    printf("a world refuses a peer that presents no certificate at all\n");

    Pair pair = connect_pair();
    WorldSide w = {.fd = pair.world_fd, .ctx = g_world_ctx, .pins = &g_realm_pins};
    pthread_t t;
    pthread_create(&t, NULL, world_thread, &w);

    /* A plain client context: it presents nothing. Without
     * SSL_VERIFY_FAIL_IF_NO_PEER_CERT this reaches the pin check with no
     * certificate to fingerprint, which is a subtler way of arriving at the
     * same place -- so this asserts the handshake itself is refused. */
    SSL_CTX* bare = tls_fixture_client_ctx();
    SSL* ssl = SSL_new(bare);
    SSL_set_fd(ssl, pair.realm_fd);
    SSL_connect(ssl);
    pthread_join(t, NULL);

    check(w.result == NULL, "the world refused it");

    SSL_free(ssl);
    SSL_CTX_free(bare);
    close(pair.realm_fd);
    close(pair.world_fd);
}

static void test_an_empty_pin_set_accepts_nothing(void) {
    printf("an empty pin set refuses every peer rather than accepting any\n");

    CertPinSet empty;
    cert_pin_reset(&empty);

    Pair pair = connect_pair();
    WorldSide w = {.fd = pair.world_fd, .ctx = g_world_ctx, .pins = &empty};
    pthread_t t;
    pthread_create(&t, NULL, world_thread, &w);

    SSL* realm = tls_connect_pinned(g_realm_ctx, pair.realm_fd, &g_world_pins,
                                    "world-under-test", 5000);
    pthread_join(t, NULL);

    /* "No pins configured" must not read as "no restriction". A pin file that
     * failed to load is the case this protects: the server refuses to start on
     * one, but the check itself has to fail closed too. */
    check(w.result == NULL, "the world with no pins accepted nobody");

    if (realm) tls_close(realm);
    close(pair.realm_fd);
    close(pair.world_fd);
}

int main(void) {
    printf("=== realm <-> world TLS ===\n");
    signal(SIGPIPE, SIG_IGN);

    log_init();
    log_set_level(LOG_LEVEL_ERROR);

    /* Every context here is built by the production function, from PEM files on
     * disk and a pin file read by cert_pin_load_file(). Reaching past those with
     * SSL_CTX_use_certificate() would leave the parts an operator actually gets
     * wrong -- a key that does not match its certificate, a pin file with a
     * typo -- untested. */
    tls_fixture_identity(&g_world_id,    "mmo-world-test");
    tls_fixture_identity(&g_realm_id,    "mmo-realm-test");
    tls_fixture_identity(&g_impostor_id, "mmo-impostor");

    tls_fixture_write_pem(&g_world_id,    W_CRT, W_KEY);
    tls_fixture_write_pem(&g_realm_id,    R_CRT, R_KEY);
    tls_fixture_write_pem(&g_impostor_id, I_CRT, I_KEY);
    tls_fixture_write_pin_file(&g_realm_id, R_PINS);
    tls_fixture_write_pin_file(&g_world_id, W_PINS);

    g_world_ctx = tls_server_init_mutual(W_CRT, W_KEY);
    assert(g_world_ctx);
    g_realm_ctx = tls_client_ctx_init(R_CRT, R_KEY);
    assert(g_realm_ctx);

    /* The impostor is used from both ends -- as a realm presenting an unpinned
     * key, and as a world doing the same -- so it needs both methods. Two
     * contexts over one identity. */
    g_impostor_ctx = tls_client_ctx_init(I_CRT, I_KEY);
    assert(g_impostor_ctx);
    g_impostor_srv = tls_server_init(I_CRT, I_KEY);
    assert(g_impostor_srv);

    char reason[256];
    cert_pin_reset(&g_realm_pins);
    cert_pin_reset(&g_world_pins);
    check(cert_pin_load_file(&g_realm_pins, R_PINS, reason, sizeof(reason)),
          "the world's realm pin file loaded");
    check(cert_pin_load_file(&g_world_pins, W_PINS, reason, sizeof(reason)),
          "the realm's world pin file loaded");

    start_listener();

    test_a_pinned_pair_completes();
    test_two_packets_in_one_record();
    test_an_unpinned_realm_is_refused();
    test_an_unpinned_world_is_refused();
    test_a_peer_with_no_certificate_is_refused();
    test_an_empty_pin_set_accepts_nothing();

    close(g_listen_fd);
    unlink(W_CRT); unlink(W_KEY);
    unlink(R_CRT); unlink(R_KEY);
    unlink(I_CRT); unlink(I_KEY);
    unlink(R_PINS); unlink(W_PINS);
    SSL_CTX_free(g_impostor_srv);
    SSL_CTX_free(g_impostor_ctx);
    SSL_CTX_free(g_realm_ctx);
    SSL_CTX_free(g_world_ctx);
    tls_fixture_identity_free(&g_impostor_id);
    tls_fixture_identity_free(&g_realm_id);
    tls_fixture_identity_free(&g_world_id);

    printf("\n%d checks passed\n", atomic_load(&passed));
    return 0;
}
