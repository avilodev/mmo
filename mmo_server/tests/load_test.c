#define _GNU_SOURCE

/**
 * @file
 * Measure the three services under concurrent load, at each junction separately.
 *
 * The point is not a single headline number. Each service is bounded by a
 * different thing -- the login server by password hashing, the realm by a
 * four-connection database pool, the world by its tick budget -- so a combined
 * figure would just report whichever one is tightest and hide the rest.
 *
 * Every stage is therefore timed on its own, and reported as a distribution
 * rather than a mean. A mean hides the thing that actually matters under load:
 * a service that answers most requests in 2ms and the unlucky one in 4 seconds
 * has a fine average and a queue problem.
 *
 *   ./load_test login   <clients> <requests>   TLS handshake + credential check
 *   ./load_test realm   <clients> <requests>   authenticated character queries
 *   ./load_test journey <clients>              login -> realm -> world, once each
 *
 * Point it only at a server you are willing to hammer.
 */

#include "protocol.h"
#include "types.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define MAX_CLIENTS 512
#define MAX_SAMPLES 200000

static SSL_CTX* g_ctx;

/* --- Timing -------------------------------------------------------------- */

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1000000.0;
}

/** Collect latencies from every thread, then report the shape of them. */
static struct {
    double        samples[MAX_SAMPLES];
    atomic_int    count;
    atomic_int    errors;
} g_stats;

static void record(double ms) {
    int slot = atomic_fetch_add(&g_stats.count, 1);
    if (slot < MAX_SAMPLES) g_stats.samples[slot] = ms;
}

static int compare_double(const void* a, const void* b) {
    double x = *(const double*)a, y = *(const double*)b;
    return (x > y) - (x < y);
}

/** Print throughput and the tail, which is what load actually changes. */
static void report(const char* what, double elapsed_ms, int clients) {
    int n = atomic_load(&g_stats.count);
    if (n > MAX_SAMPLES) n = MAX_SAMPLES;
    int errors = atomic_load(&g_stats.errors);

    printf("\n  %s — %d clients\n", what, clients);
    if (n == 0) {
        printf("    no successful requests (%d errors)\n", errors);
        return;
    }

    qsort(g_stats.samples, (size_t)n, sizeof(double), compare_double);

    double total = 0;
    for (int i = 0; i < n; i++) total += g_stats.samples[i];

    printf("    %-22s %d in %.0f ms\n", "completed", n, elapsed_ms);
    printf("    %-22s %.0f/s\n", "throughput", n * 1000.0 / elapsed_ms);
    printf("    %-22s %.1f ms\n", "mean", total / n);
    printf("    %-22s %.1f ms\n", "p50", g_stats.samples[n / 2]);
    printf("    %-22s %.1f ms\n", "p95", g_stats.samples[(int)(n * 0.95)]);
    printf("    %-22s %.1f ms\n", "p99", g_stats.samples[(int)(n * 0.99)]);
    printf("    %-22s %.1f ms\n", "max", g_stats.samples[n - 1]);
    printf("    %-22s %d\n", "errors", errors);
}

/* --- Sockets ------------------------------------------------------------- */

static int tcp_connect(const char* ip, uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    inet_pton(AF_INET, ip, &addr.sin_addr);

    struct timeval tv = { .tv_sec = 20, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) { close(fd); return -1; }
    return fd;
}

/** Read exactly one framed packet. */
static int read_packet(int fd, void* out, size_t cap) {
    uint8_t* buf = out;
    size_t have = 0;

    while (have < sizeof(PacketHeader)) {
        ssize_t n = recv(fd, buf + have, sizeof(PacketHeader) - have, 0);
        if (n <= 0) return -1;
        have += (size_t)n;
    }

    size_t want = sizeof(PacketHeader) + ntohs(((PacketHeader*)buf)->payload_size);
    if (want > cap) return -1;

    while (have < want) {
        ssize_t n = recv(fd, buf + have, want - have, 0);
        if (n <= 0) return -1;
        have += (size_t)n;
    }
    return (int)have;
}

/* --- Login --------------------------------------------------------------- */

/** One TLS session against the login server, torn down afterwards. */
typedef struct { int fd; SSL* ssl; } Tls;

static int tls_open(Tls* out) {
    out->fd = tcp_connect("127.0.0.1", LOGIN_SERVER_PORT);
    if (out->fd < 0) return 0;

    out->ssl = SSL_new(g_ctx);
    if (!out->ssl) { close(out->fd); return 0; }
    SSL_set_fd(out->ssl, out->fd);

    if (SSL_connect(out->ssl) != 1) {
        SSL_free(out->ssl); close(out->fd);
        out->ssl = NULL; out->fd = -1;
        return 0;
    }
    return 1;
}

static void tls_shut(Tls* t) {
    if (t->ssl) { SSL_free(t->ssl); t->ssl = NULL; }
    if (t->fd >= 0) { close(t->fd); t->fd = -1; }
}

static int do_login(const char* user, uint32_t* out_player, char out_token[32]);

/** Make sure an account exists, without spending a failed attempt on it.
 *
 * Registering an account that already exists is a *failure* as far as the login
 * server is concerned, and five failures from one address inside a minute earn
 * a five-minute block. A harness that blindly re-registers therefore locks
 * itself out before it measures anything -- which it did, and which looked
 * exactly like the server falling over under load.
 */
static void ensure_account(const char* user) {
    if (do_login(user, NULL, NULL)) return;   /* already there */

    Tls t;
    if (!tls_open(&t)) return;

    AuthRegisterPacket reg;
    memset(&reg, 0, sizeof(reg));
    reg.header.type         = PACKET_AUTH_REGISTER;
    reg.header.payload_size = htons(sizeof(reg) - sizeof(PacketHeader));
    snprintf(reg.username, sizeof(reg.username), "%s", user);
    snprintf(reg.password, sizeof(reg.password), "LoadTest123");
    snprintf(reg.email,    sizeof(reg.email),    "%s@example.invalid", user);
    snprintf(reg.birthday, sizeof(reg.birthday), "1990-01-01");
    SSL_write(t.ssl, &reg, (int)sizeof(reg));

    uint8_t buf[512];
    SSL_read(t.ssl, buf, sizeof(buf));
    tls_shut(&t);
}

/**
 * One full login: TCP connect, TLS handshake, credential check, reply.
 *
 * @return 1 on an accepted login.
 */
static int do_login(const char* user, uint32_t* out_player, char out_token[32]) {
    Tls t;
    if (!tls_open(&t)) return 0;

    AuthLoginPacket login;
    memset(&login, 0, sizeof(login));
    login.header.type         = PACKET_AUTH_LOGIN;
    login.header.payload_size = htons(sizeof(login) - sizeof(PacketHeader));
    snprintf(login.username, sizeof(login.username), "%s", user);
    snprintf(login.password, sizeof(login.password), "LoadTest123");

    if (SSL_write(t.ssl, &login, (int)sizeof(login)) != (int)sizeof(login)) {
        tls_shut(&t); return 0;
    }

    uint8_t buf[512];
    int got = SSL_read(t.ssl, buf, sizeof(buf));
    int ok = got >= 44 && buf[7] == 1;
    if (ok) {
        if (out_player) *out_player = ntohl(*(uint32_t*)(buf + 1));
        if (out_token)  memcpy(out_token, buf + 12, 32);
    }
    tls_shut(&t);
    return ok;
}

/** Trade an auth token for a realm session key. */
static int do_start_game(const char* user, const char token[32], uint8_t out_key[32]) {
    Tls t;
    if (!tls_open(&t)) return 0;

    StartGameRequestPacket req;
    memset(&req, 0, sizeof(req));
    req.header.type         = PACKET_START_GAME_REQUEST;
    req.header.payload_size = htons(sizeof(req) - sizeof(PacketHeader));
    snprintf(req.username, sizeof(req.username), "%s", user);
    memcpy(req.auth_token, token, 32);

    SSL_write(t.ssl, &req, (int)sizeof(req));

    uint8_t buf[512];
    int got = SSL_read(t.ssl, buf, sizeof(buf));
    int ok = got >= 40 && buf[39] == 1;
    if (ok) memcpy(out_key, buf + 7, 32);
    tls_shut(&t);
    return ok;
}

/* --- Worker threads ------------------------------------------------------ */

typedef struct {
    int  id;
    int  requests;
    char user[32];
} Job;

/** Hammer the login server: handshake plus credential check, repeatedly. */
static void* login_worker(void* arg) {
    Job* job = arg;

    for (int i = 0; i < job->requests; i++) {
        double start = now_ms();
        if (do_login(job->user, NULL, NULL)) record(now_ms() - start);
        else atomic_fetch_add(&g_stats.errors, 1);
    }
    return NULL;
}

/** Authenticate to the realm once, then time repeated character-list queries. */
static void* realm_worker(void* arg) {
    Job* job = arg;

    uint32_t player_id = 0;
    char token[32];
    uint8_t key[32];

    if (!do_login(job->user, &player_id, token) ||
        !do_start_game(job->user, token, key)) {
        atomic_fetch_add(&g_stats.errors, job->requests);
        return NULL;
    }

    int fd = tcp_connect("127.0.0.1", REALM_SERVER_PORT);
    if (fd < 0) { atomic_fetch_add(&g_stats.errors, job->requests); return NULL; }

    RealmConnectPacket connect_pkt;
    memset(&connect_pkt, 0, sizeof(connect_pkt));
    connect_pkt.header.type      = PACKET_REALM_CONNECT;
    connect_pkt.header.player_id = htonl(player_id);
    memcpy(connect_pkt.header.session_key, key, 32);
    connect_pkt.protocol_version = htons(PROTOCOL_VERSION);
    send(fd, &connect_pkt, sizeof(connect_pkt), 0);

    uint8_t buf[8192];
    if (read_packet(fd, buf, sizeof(buf)) < 0 || buf[0] != PACKET_REALM_CONNECT_ACK) {
        atomic_fetch_add(&g_stats.errors, job->requests);
        close(fd);
        return NULL;
    }

    /* The world list first, so the character query below has a world to name. */
    WorldListRequestPacket wl;
    memset(&wl, 0, sizeof(wl));
    wl.header.type         = PACKET_WORLD_LIST_REQUEST;
    wl.header.player_id    = htonl(player_id);
    wl.header.payload_size = htons(sizeof(wl) - sizeof(PacketHeader));
    send(fd, &wl, sizeof(wl), 0);

    uint32_t world_id = 0;
    if (read_packet(fd, buf, sizeof(buf)) > 0 && buf[0] == PACKET_WORLD_LIST_RESPONSE) {
        WorldListResponsePacket* list = (WorldListResponsePacket*)buf;
        for (int i = 0; i < list->count && i < MAX_WORLDS; i++)
            if (list->worlds[i].status == 1) { world_id = ntohl(list->worlds[i].world_id); break; }
    }

    /* This is the measured request: a character list, which is one PostgreSQL
     * round trip through the shared connection pool. */
    for (int i = 0; i < job->requests; i++) {
        CharacterListRequestPacket req;
        memset(&req, 0, sizeof(req));
        req.header.type         = PACKET_CHARACTER_LIST_REQUEST;
        req.header.player_id    = htonl(player_id);
        req.header.payload_size = htons(sizeof(req) - sizeof(PacketHeader));
        req.world_id            = htonl(world_id);

        double start = now_ms();
        if (send(fd, &req, sizeof(req), 0) != (ssize_t)sizeof(req)) {
            atomic_fetch_add(&g_stats.errors, 1);
            break;
        }
        if (read_packet(fd, buf, sizeof(buf)) < 0) {
            atomic_fetch_add(&g_stats.errors, 1);
            break;
        }
        record(now_ms() - start);
    }

    close(fd);
    return NULL;
}

/** Walk the whole path once, timing it end to end. */
static void* journey_worker(void* arg) {
    Job* job = arg;
    double start = now_ms();

    uint32_t player_id = 0;
    char token[32];
    uint8_t key[32];

    if (!do_login(job->user, &player_id, token) ||
        !do_start_game(job->user, token, key)) {
        atomic_fetch_add(&g_stats.errors, 1);
        return NULL;
    }

    int fd = tcp_connect("127.0.0.1", REALM_SERVER_PORT);
    if (fd < 0) { atomic_fetch_add(&g_stats.errors, 1); return NULL; }

    RealmConnectPacket connect_pkt;
    memset(&connect_pkt, 0, sizeof(connect_pkt));
    connect_pkt.header.type      = PACKET_REALM_CONNECT;
    connect_pkt.header.player_id = htonl(player_id);
    memcpy(connect_pkt.header.session_key, key, 32);
    connect_pkt.protocol_version = htons(PROTOCOL_VERSION);
    send(fd, &connect_pkt, sizeof(connect_pkt), 0);

    uint8_t buf[8192];
    int ok = read_packet(fd, buf, sizeof(buf)) > 0 && buf[0] == PACKET_REALM_CONNECT_ACK
             && ((RealmConnectAckPacket*)buf)->success;
    close(fd);

    if (ok) record(now_ms() - start);
    else    atomic_fetch_add(&g_stats.errors, 1);
    return NULL;
}

/* --- Main ---------------------------------------------------------------- */

int main(int argc, char** argv) {
    signal(SIGPIPE, SIG_IGN);

    const char* mode = argc > 1 ? argv[1] : "login";
    int clients  = argc > 2 ? atoi(argv[2]) : 16;
    int requests = argc > 3 ? atoi(argv[3]) : 10;

    if (clients < 1) clients = 1;
    if (clients > MAX_CLIENTS) clients = MAX_CLIENTS;
    if (requests < 1) requests = 1;

    SSL_library_init();
    g_ctx = SSL_CTX_new(TLS_client_method());
    if (!g_ctx) { fprintf(stderr, "cannot create a TLS context\n"); return 1; }
    SSL_CTX_set_verify(g_ctx, SSL_VERIFY_NONE, NULL);

    void* (*worker)(void*) =
        strcmp(mode, "realm")   == 0 ? realm_worker :
        strcmp(mode, "journey") == 0 ? journey_worker : login_worker;

    if (strcmp(mode, "journey") == 0) requests = 1;

    /* Accounts are created up front and outside the measurement: registration
     * is a write, and the shape being measured is the read path. */
    static Job jobs[MAX_CLIENTS];
    for (int i = 0; i < clients; i++) {
        jobs[i].id = i;
        jobs[i].requests = requests;
        snprintf(jobs[i].user, sizeof(jobs[i].user), "load_%d", i);
        ensure_account(jobs[i].user);
    }

    printf("=== %s: %d clients x %d requests ===\n", mode, clients, requests);

    static pthread_t threads[MAX_CLIENTS];
    double start = now_ms();
    for (int i = 0; i < clients; i++)
        pthread_create(&threads[i], NULL, worker, &jobs[i]);
    for (int i = 0; i < clients; i++)
        pthread_join(threads[i], NULL);
    double elapsed = now_ms() - start;

    report(mode, elapsed, clients);
    SSL_CTX_free(g_ctx);
    return 0;
}
