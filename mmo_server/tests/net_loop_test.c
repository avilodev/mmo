// Integration test for the epoll event loops.
//
// Drives real loopback sockets through net_loop with the database and gameplay
// layers stubbed out, because those are not what this test is checking. What it
// checks is the part that was rewritten: connection handoff between loop and
// worker, reassembly across TCP segment boundaries, dispatch, teardown, and
// whether many concurrent connections actually work on a handful of threads.
//
// Build: see the net_loop_test target in the Makefile.

#include "net_loop.h"
#include "packet_limiter.h"
#include "limit_profiles.h"
#include "connection_io.h"
#include "protocol.h"
#include "log.h"

#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

// ---------------------------------------------------------------------------
// Test harness
// ---------------------------------------------------------------------------

static int g_listen_fd = -1;
static int g_listen_port = 0;

static atomic_int g_packets_dispatched = 0;
static atomic_int g_auth_calls         = 0;

static void start_listener(void) {
    g_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(g_listen_fd >= 0);
    int opt = 1;
    setsockopt(g_listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;   // let the kernel pick
    assert(bind(g_listen_fd, (struct sockaddr*)&addr, sizeof(addr)) == 0);
    assert(listen(g_listen_fd, 512) == 0);

    socklen_t len = sizeof(addr);
    assert(getsockname(g_listen_fd, (struct sockaddr*)&addr, &len) == 0);
    g_listen_port = ntohs(addr.sin_port);
}

// Connect a client and hand the server side to the event loops, exactly as the
// real accept thread does.
static int connect_client(void) {
    int c = socket(AF_INET, SOCK_STREAM, 0);
    assert(c >= 0);
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((uint16_t)g_listen_port);
    assert(connect(c, (struct sockaddr*)&addr, sizeof(addr)) == 0);

    int s = accept(g_listen_fd, NULL, NULL);
    assert(s >= 0);
    net_loop_submit(s);
    return c;
}

static void send_auth(int c) {
    WorldConnectPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.header.type = PACKET_WORLD_CONNECT;
    pkt.header.payload_size = htons(sizeof(pkt) - sizeof(PacketHeader));
    strncpy(pkt.game_ticket, "test-ticket", sizeof(pkt.game_ticket) - 1);
    assert(send(c, &pkt, sizeof(pkt), 0) == (ssize_t)sizeof(pkt));
}

// Read with a bounded wait so a hang fails the test instead of stalling it.
static ssize_t recv_timeout(int fd, void* buf, size_t len, int ms) {
    struct timeval tv = { .tv_sec = ms / 1000, .tv_usec = (ms % 1000) * 1000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    return recv(fd, buf, len, 0);
}

static void wait_for(atomic_int* counter, int target, int ms) {
    for (int i = 0; i < ms / 5 && atomic_load(counter) < target; i++)
        usleep(5000);
}

// ---------------------------------------------------------------------------
// Stubs: database and gameplay layers this test is not exercising
// ---------------------------------------------------------------------------

int validate_game_ticket(const char* t, uint32_t* acct, uint32_t* chr, uint32_t* world) {
    atomic_fetch_add(&g_auth_calls, 1);
    if (!t || strncmp(t, "test-ticket", 11) != 0) return 0;
    *acct = 1000; *chr = 2000; *world = 1;
    return 1;
}
uint32_t get_account_from_ticket(const char* t) { (void)t; return 1000; }
uint32_t character_get_owner(uint32_t c) { (void)c; return 1000; }

int  session_registry_add(int fd, uint32_t a, uint32_t c) { (void)fd;(void)a;(void)c; return 0; }
void session_registry_remove(int fd) { (void)fd; }
void session_update_activity(int fd) { (void)fd; }

int  player_add_active(uint32_t c, int fd) { (void)c;(void)fd; return 1; }
int  player_remove_active_if_fd(uint32_t c, int fd) { (void)c;(void)fd; return 1; }
void player_send_data_response(int fd, uint32_t c) { (void)fd;(void)c; }

// Returning NULL sends worker_cleanup down its "slot already gone" path, which
// is the branch that does no database work — correct for a stubbed run.
struct ActivePlayer;
struct ActivePlayer* player_acquire(uint32_t c) { (void)c; return NULL; }
void player_release(struct ActivePlayer* p) { (void)p; }
void player_send_stats_locked(int fd, struct ActivePlayer* p) { (void)fd;(void)p; }
void ability_send_data(int fd, struct ActivePlayer* p) { (void)fd;(void)p; }

void quest_send_all(uint32_t c, int fd) { (void)c;(void)fd; }
int  quest_player_save(uint32_t c, const void* q, int n) { (void)c;(void)q;(void)n; return 1; }
int  character_update_full_data(const void* info) { (void)info; return 1; }
void party_handle_disconnect(uint32_t c) { (void)c; }

// The unit under test only cares that dispatch happens with correct framing.
int process_packet(int fd, uint32_t character_id, ssize_t bytes, uint8_t* buffer) {
    (void)character_id;
    PacketHeader* h = (PacketHeader*)buffer;

    // Framing must be exact: a mis-framed stream would show up as a size that
    // disagrees with the header.
    assert(bytes == (ssize_t)(sizeof(PacketHeader) + ntohs(h->payload_size)));

    atomic_fetch_add(&g_packets_dispatched, 1);
    if (h->type == PACKET_LOGOUT) return -1;

    switch (packet_limiter_check(fd, h->type)) {
        case PACKET_LIMIT_KICK: return -1;
        case PACKET_LIMIT_DROP: return 0;
        default: break;
    }
    return 1;
}

// g_server / g_state live in world_server/src/config.c, which this test does
// not link (it would drag in the socket setup). Provide them here.
#include "config.h"
ServerConfig g_server;
ServerState  g_state;

// ---------------------------------------------------------------------------

int main(void) {
    log_init();
    log_set_level(LOG_LEVEL_ERROR);

    connection_io_init();
    packet_limiter_init(limit_profile_world());
    start_listener();
    assert(net_loop_start() == 0);

    // ------------------------------------------------------------------
    printf("TEST 1: a client authenticates and receives its ack\n");
    int c1 = connect_client();
    send_auth(c1);

    WorldConnectAckPacket ack;
    ssize_t got = recv_timeout(c1, &ack, sizeof(ack), 2000);
    printf("  ack bytes=%zd success=%d msg=\"%s\"\n",
           got, got > 0 ? ack.success : -1, got > 0 ? ack.welcome_message : "");
    assert(got == (ssize_t)sizeof(ack));
    assert(ack.success == 1);
    assert(net_loop_connection_count() == 1);

    // ------------------------------------------------------------------
    printf("\nTEST 2: packets dispatch with correct framing\n");
    atomic_store(&g_packets_dispatched, 0);
    for (int i = 0; i < 20; i++) {
        PacketHeader ping = { .type = PACKET_PING, .player_id = 0, .payload_size = htons(0) };
        assert(send(c1, &ping, sizeof(ping), 0) == (ssize_t)sizeof(ping));
    }
    wait_for(&g_packets_dispatched, 20, 2000);
    printf("  dispatched=%d of 20\n", atomic_load(&g_packets_dispatched));
    assert(atomic_load(&g_packets_dispatched) == 20);

    // ------------------------------------------------------------------
    printf("\nTEST 3: a packet split across two writes is reassembled\n");
    atomic_store(&g_packets_dispatched, 0);
    {
        PacketHeader ping = { .type = PACKET_PING, .player_id = 0, .payload_size = htons(0) };
        uint8_t* raw = (uint8_t*)&ping;
        // Deliberately split mid-header, then pause so the loop sees a partial
        // read and has to carry it over.
        assert(send(c1, raw, 3, 0) == 3);
        usleep(150000);
        assert(atomic_load(&g_packets_dispatched) == 0);   // must not fire early
        assert(send(c1, raw + 3, sizeof(ping) - 3, 0) == (ssize_t)(sizeof(ping) - 3));
    }
    wait_for(&g_packets_dispatched, 1, 2000);
    printf("  dispatched after the second write: %d (expect 1)\n",
           atomic_load(&g_packets_dispatched));
    assert(atomic_load(&g_packets_dispatched) == 1);

    // ------------------------------------------------------------------
    printf("\nTEST 4: several packets coalesced into one write all dispatch\n");
    atomic_store(&g_packets_dispatched, 0);
    {
        uint8_t batch[sizeof(PacketHeader) * 5];
        for (int i = 0; i < 5; i++) {
            PacketHeader h = { .type = PACKET_PING, .player_id = 0, .payload_size = htons(0) };
            memcpy(batch + i * sizeof(PacketHeader), &h, sizeof(h));
        }
        assert(send(c1, batch, sizeof(batch), 0) == (ssize_t)sizeof(batch));
    }
    wait_for(&g_packets_dispatched, 5, 2000);
    printf("  dispatched=%d of 5 coalesced\n", atomic_load(&g_packets_dispatched));
    assert(atomic_load(&g_packets_dispatched) == 5);

    // ------------------------------------------------------------------
    printf("\nTEST 5: client disconnect tears the connection down\n");
    close(c1);
    for (int i = 0; i < 400 && net_loop_connection_count() > 0; i++) usleep(5000);
    printf("  live connections after close: %d (expect 0)\n", net_loop_connection_count());
    assert(net_loop_connection_count() == 0);

    // ------------------------------------------------------------------
    printf("\nTEST 6: an oversized packet is refused with a reason\n");
    int c2 = connect_client();
    send_auth(c2);
    got = recv_timeout(c2, &ack, sizeof(ack), 2000);
    assert(got == (ssize_t)sizeof(ack) && ack.success == 1);
    {
        // Claim a payload larger than MAX_PACKET_SIZE.
        PacketHeader bad = { .type = PACKET_PING, .player_id = 0,
                             .payload_size = htons(60000) };
        assert(send(c2, &bad, sizeof(bad), 0) == (ssize_t)sizeof(bad));
    }
    {
        DisconnectPacket dc;
        ssize_t n = recv_timeout(c2, &dc, sizeof(dc), 2000);
        printf("  got %zd bytes, type=%d reason=%d msg=\"%s\"\n",
               n, n > 0 ? dc.header.type : -1, n > 0 ? dc.reason : -1,
               n > 0 ? dc.message : "");
        assert(n == (ssize_t)sizeof(dc));
        assert(dc.header.type == PACKET_DISCONNECT);
        assert(dc.reason == DISCONNECT_REASON_PROTOCOL);
    }
    close(c2);
    for (int i = 0; i < 400 && net_loop_connection_count() > 0; i++) usleep(5000);
    assert(net_loop_connection_count() == 0);

    // ------------------------------------------------------------------
    printf("\nTEST 7: 250 concurrent connections on a handful of threads\n");
    // The whole point of the migration: connection count is no longer bounded
    // by thread count. Under the old pool this needed 250 threads.
    enum { MANY = 250 };
    int clients[MANY];
    atomic_store(&g_packets_dispatched, 0);

    for (int i = 0; i < MANY; i++) {
        clients[i] = connect_client();
        send_auth(clients[i]);
    }
    for (int i = 0; i < MANY; i++) {
        ssize_t n = recv_timeout(clients[i], &ack, sizeof(ack), 3000);
        assert(n == (ssize_t)sizeof(ack));
        assert(ack.success == 1);
    }
    printf("  all %d authenticated, live=%d\n", MANY, net_loop_connection_count());
    assert(net_loop_connection_count() == MANY);

    // Every connection sends traffic at once.
    for (int i = 0; i < MANY; i++) {
        PacketHeader ping = { .type = PACKET_PING, .player_id = 0, .payload_size = htons(0) };
        send(clients[i], &ping, sizeof(ping), 0);
    }
    wait_for(&g_packets_dispatched, MANY, 5000);
    printf("  packets dispatched across all connections: %d of %d\n",
           atomic_load(&g_packets_dispatched), MANY);
    assert(atomic_load(&g_packets_dispatched) == MANY);

    // How many threads are actually servicing them?
    {
        FILE* f = fopen("/proc/self/status", "r");
        char line[128];
        int threads = -1;
        while (f && fgets(line, sizeof(line), f))
            if (sscanf(line, "Threads: %d", &threads) == 1) break;
        if (f) fclose(f);
        printf("  process threads: %d (loops + workers + main, NOT %d)\n",
               threads, MANY);
        assert(threads > 0 && threads < MANY);
    }

    for (int i = 0; i < MANY; i++) close(clients[i]);
    for (int i = 0; i < 1000 && net_loop_connection_count() > 0; i++) usleep(5000);
    printf("  live after closing all: %d (expect 0)\n", net_loop_connection_count());
    assert(net_loop_connection_count() == 0);

    // ------------------------------------------------------------------
    printf("\nTEST 8: a bad ticket is rejected, not admitted\n");
    int c3 = connect_client();
    {
        WorldConnectPacket pkt;
        memset(&pkt, 0, sizeof(pkt));
        pkt.header.type = PACKET_WORLD_CONNECT;
        pkt.header.payload_size = htons(sizeof(pkt) - sizeof(PacketHeader));
        strncpy(pkt.game_ticket, "forged", sizeof(pkt.game_ticket) - 1);
        assert(send(c3, &pkt, sizeof(pkt), 0) == (ssize_t)sizeof(pkt));
    }
    got = recv_timeout(c3, &ack, sizeof(ack), 2000);
    printf("  ack success=%d msg=\"%s\" (expect 0)\n",
           got > 0 ? ack.success : -1, got > 0 ? ack.welcome_message : "");
    assert(got == (ssize_t)sizeof(ack));
    assert(ack.success == 0);
    close(c3);
    for (int i = 0; i < 400 && net_loop_connection_count() > 0; i++) usleep(5000);
    assert(net_loop_connection_count() == 0);

    net_loop_stop();
    close(g_listen_fd);

    printf("\nALL ASSERTIONS PASSED\n");
    return 0;
}
