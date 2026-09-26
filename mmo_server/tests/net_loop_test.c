/**
 * @file
 * Check epoll handoff, TCP framing, dispatch, teardown, and concurrent connections.
 */

#include "net_loop.h"
#include "packet_limiter.h"
#include "limit_profiles.h"
#include "connection_io.h"
#include "session_registry.h"
#include "server_types.h"
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

static int g_listen_fd = -1;
static int g_listen_port = 0;

static atomic_int g_packets_dispatched = 0;
static atomic_int g_auth_calls         = 0;
/** Tickets that reached validate_game_ticket() with no NUL inside the field. */
static atomic_int g_unterminated_tickets = 0;

/** Create a loopback listener on an ephemeral port. */
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

/** Connect a loopback client and submit its accepted peer to the event loop. */
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

/** Send a valid test authentication packet for one identity.
 *
 * The suffix selects who the ticket is for; see validate_game_ticket() below.
 * It matters because session_registry.c is linked for real, and the registry's
 * whole job on a duplicate is to kick the older session -- so a test that
 * wants many simultaneous connections needs many identities, and a test that
 * wants a reconnect needs the same one twice.
 */
static void send_auth_as(int c, int identity) {
    WorldConnectPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.header.type = PACKET_WORLD_CONNECT;
    pkt.header.payload_size = htons(sizeof(pkt) - sizeof(PacketHeader));
    snprintf(pkt.game_ticket, sizeof(pkt.game_ticket), "test-ticket-%d", identity);
    pkt.protocol_version = htons(PROTOCOL_VERSION);
    assert(send(c, &pkt, sizeof(pkt), 0) == (ssize_t)sizeof(pkt));
}

/** Send the authentication packet for identity 0. */
static void send_auth(int c) { send_auth_as(c, 0); }

/** Receive with a bounded timeout in milliseconds. */
static ssize_t recv_timeout(int fd, void* buf, size_t len, int ms) {
    struct timeval tv = { .tv_sec = ms / 1000, .tv_usec = (ms % 1000) * 1000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    return recv(fd, buf, len, 0);
}

/** Poll an atomic counter until it reaches a target or timeout. */
static void wait_for(atomic_int* counter, int target, int ms) {
    for (int i = 0; i < ms / 5 && atomic_load(counter) < target; i++)
        usleep(5000);
}

/** Validate the sole ticket accepted by the event-loop fixture. */
int validate_game_ticket(const char* t, const char* peer_ip, uint32_t expected_world,
                         uint32_t* acct, uint32_t* chr, uint32_t* world) {
    (void)peer_ip;         /* the fixture connects over loopback; binding is covered elsewhere */
    (void)expected_world;  /* the fixture runs one world; world binding is covered elsewhere */
    atomic_fetch_add(&g_auth_calls, 1);
    if (!t) return 0;

    /* The caller's obligation, checked where the obligation lands.
     *
     * game_ticket is a fixed char[64] taken straight off the wire with no
     * guarantee of a NUL anywhere in it, and the real validate_game_ticket()
     * builds a Redis key from it with "%s" -- which reads to a terminator
     * regardless of the output bound. A client that filled the field therefore
     * walked that read past it, through the rest of the packet and on into the
     * reactor's buffer, before authenticating anything. See TEST 13. */
    if (!memchr(t, '\0', sizeof(((WorldConnectPacket*)0)->game_ticket)))
        atomic_fetch_add(&g_unterminated_tickets, 1);

    if (strncmp(t, "test-ticket", 11) != 0) return 0;

    /* "test-ticket-N" is identity N. Distinct identities are what let TEST 7
     * hold 250 connections open at once: the session registry is linked for
     * real here, and it resolves a duplicate account by kicking the older
     * session -- so 250 tickets for one account would leave one connection,
     * not 250. */
    int identity = 0;
    const char* dash = t + 11;
    if (*dash == '-') identity = (int)strtol(dash + 1, NULL, 10);

    *acct  = 1000 + (uint32_t)identity;
    *chr   = 2000 + (uint32_t)identity;
    *world = 1;
    return 1;
}
/** Stub recording that a character is live in a world. */
int world_session_mark(uint32_t character_id, uint32_t world_id) {
    (void)character_id; (void)world_id; return 1;
}
/** Stub clearing that record. */
void world_session_clear(uint32_t character_id) { (void)character_id; }

/** Stub the database pool size the worker count is derived from.
 *
 * The real one reads $MMO_DB_POOL_SIZE; this test opens no database, and what
 * it cares about is that net_loop_start() asks for a plausible worker count,
 * not which number it gets. */
int character_database_pool_configured_size(void);
int character_database_pool_configured_size(void) { return 4; }

/** The account that owns a test character, matching validate_game_ticket(). */
uint32_t character_get_owner(uint32_t c) { return 1000 + (c - 2000); }

/* session_registry.c is linked for real rather than stubbed.
 *
 * It is where a duplicate login is resolved -- the stale session is kicked so
 * the new one can proceed -- and that is a behaviour with no test at all while
 * the registry is a stub that returns 0. It is self-contained (a table sized
 * from RLIMIT_NOFILE, two indexes and shutdown(2)), so linking it costs
 * nothing and turns TEST 10 and TEST 11 below into an actual reconnect. */

/** The active-player roster.
 *
 * The real one loads a character out of PostgreSQL. What the reconnect cases
 * need is the bookkeeping the teardown path in net_loop.c actually consults:
 * which descriptor owns a character right now. That path acquires the player,
 * compares `client_fd` against its own, and removes the entry only when the
 * two match -- which is the whole of how a kicked session avoids evicting the
 * session that replaced it. A stub that returned NULL from player_acquire()
 * skipped all of it, so none of it was ever exercised.
 *
 * One mutex covers both the table and every entry: this is a test, and
 * serializing it removes a class of fixture bug without changing what is
 * being measured.
 */
#define ROSTER_MAX 512
static pthread_mutex_t g_roster_lock = PTHREAD_MUTEX_INITIALIZER;
static ActivePlayer    g_players[ROSTER_MAX];
static int             g_player_used[ROSTER_MAX];

/** Find the entry holding a character, or -1. Caller holds g_roster_lock. */
static int roster_find_locked(uint32_t character_id) {
    for (int i = 0; i < ROSTER_MAX; i++)
        if (g_player_used[i] && g_players[i].character_id == character_id) return i;
    return -1;
}

int player_add_active(uint32_t c, int fd, int* out_slot) {
    pthread_mutex_lock(&g_roster_lock);

    int slot = roster_find_locked(c);
    if (slot < 0) {
        for (int i = 0; i < ROSTER_MAX && slot < 0; i++)
            if (!g_player_used[i]) slot = i;
    }
    if (slot < 0) { pthread_mutex_unlock(&g_roster_lock); return 0; }

    memset(&g_players[slot], 0, sizeof(g_players[slot]));
    g_players[slot].character_id = c;
    g_players[slot].client_fd    = fd;
    g_players[slot].is_loaded    = 1;
    g_player_used[slot]          = 1;

    pthread_mutex_unlock(&g_roster_lock);
    if (out_slot) *out_slot = slot;
    return 1;
}

ActivePlayer* player_acquire(uint32_t c) {
    pthread_mutex_lock(&g_roster_lock);
    int slot = roster_find_locked(c);
    if (slot < 0) { pthread_mutex_unlock(&g_roster_lock); return NULL; }
    return &g_players[slot];       /* released by player_release() */
}

void player_release(ActivePlayer* p) {
    if (p) pthread_mutex_unlock(&g_roster_lock);
}

/** Remove the entry only when the descriptor still owns it.
 *
 * The real function's contract, and the point of its name: after a stale
 * session has been kicked, the old connection's teardown must not evict the
 * new one that took its place.
 */
int player_remove_active_if_fd(uint32_t c, int fd) {
    int removed = 0;
    pthread_mutex_lock(&g_roster_lock);
    int slot = roster_find_locked(c);
    if (slot >= 0 && g_players[slot].client_fd == fd) {
        g_player_used[slot] = 0;
        removed = 1;
    }
    pthread_mutex_unlock(&g_roster_lock);
    return removed;
}

/** How many characters the roster believes are in the world. */
static int roster_count(void) {
    int n = 0;
    pthread_mutex_lock(&g_roster_lock);
    for (int i = 0; i < ROSTER_MAX; i++) if (g_player_used[i]) n++;
    pthread_mutex_unlock(&g_roster_lock);
    return n;
}

/** Stub initial player-data transmission. */
void player_send_data_response(int fd, uint32_t c) { (void)fd;(void)c; }

struct PlayerSaveData;
/** Stub player save snapshot creation. */
void player_snapshot_for_save(const void* p, void* out) { (void)p; (void)out; }
/** Stub successful player save commit. */
int  player_commit_save(const void* s) { (void)s; return 1; }

/** Stub player-stat transmission. */
void player_send_stats_locked(int fd, ActivePlayer* p) { (void)fd;(void)p; }
/** Stub ability-data transmission. */
void ability_send_data(int fd, ActivePlayer* p) { (void)fd;(void)p; }

/** Stub quest-data transmission. */
void quest_send_all(uint32_t c, int fd) { (void)c;(void)fd; }
/** Stub successful quest persistence. */
int  quest_player_save(uint32_t c, const void* state) { (void)c;(void)state; return 1; }
/** Stub successful character persistence. */
int  character_update_full_data(const void* info) { (void)info; return 1; }
/** Stub party disconnect cleanup. */
void party_handle_disconnect(uint32_t c) { (void)c; }

/* The friends hooks net_loop.c calls on entry and exit.
 *
 * The real ones are world_server/src/friends.c, which pulls in the friend bus,
 * the presence cache and Redis -- none of which the reactor contract this file
 * measures has anything to do with. Stubbed rather than linked for the same
 * reason party_handle_disconnect() above is.
 */
void world_friends_player_entered(uint32_t account_id, uint32_t character_id,
                                  const char* character_name) {
    (void)account_id; (void)character_id; (void)character_name;
}
void world_friends_player_left(uint32_t account_id, uint32_t character_id) {
    (void)account_id; (void)character_id;
}

/** Validate framing and record one dispatched packet. */
int process_packet(int fd, uint32_t character_id, int player_slot,
                   ssize_t bytes, uint8_t* buffer) {
    (void)character_id;
    (void)player_slot;
    PacketHeader* h = (PacketHeader*)buffer;

    // require exact header-derived framing
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

#include "config.h"

/** Stub the whole-character save that commits scalars, currency and items together. */
int character_save_all(const CharacterInfo* d, const ItemInstance* inv, int n_inv,
                       const ItemInstance* eq, int n_eq) {
    (void)d; (void)inv; (void)n_inv; (void)eq; (void)n_eq; return 1;
}
ServerConfig g_server;
ServerState  g_state;

/**
 * Run event-loop integration and concurrency assertions.
 *
 * @return      Zero after all assertions pass.
 */
int main(void) {
    log_init();
    log_set_level(LOG_LEVEL_ERROR);

    connection_io_init();
    packet_limiter_init(limit_profile_world());
    session_registry_init();
    start_listener();
    assert(net_loop_start(0) == 0);

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

    printf("\nTEST 2: packets dispatch with correct framing\n");
    atomic_store(&g_packets_dispatched, 0);
    for (int i = 0; i < 20; i++) {
        PacketHeader ping = { .type = PACKET_PING, .player_id = 0, .payload_size = htons(0) };
        assert(send(c1, &ping, sizeof(ping), 0) == (ssize_t)sizeof(ping));
    }
    wait_for(&g_packets_dispatched, 20, 2000);
    printf("  dispatched=%d of 20\n", atomic_load(&g_packets_dispatched));
    assert(atomic_load(&g_packets_dispatched) == 20);

    printf("\nTEST 3: a packet split across two writes is reassembled\n");
    atomic_store(&g_packets_dispatched, 0);
    {
        PacketHeader ping = { .type = PACKET_PING, .player_id = 0, .payload_size = htons(0) };
        uint8_t* raw = (uint8_t*)&ping;
        // split within the header to force reassembly
        assert(send(c1, raw, 3, 0) == 3);
        usleep(150000);
        assert(atomic_load(&g_packets_dispatched) == 0);   // must not fire early
        assert(send(c1, raw + 3, sizeof(ping) - 3, 0) == (ssize_t)(sizeof(ping) - 3));
    }
    wait_for(&g_packets_dispatched, 1, 2000);
    printf("  dispatched after the second write: %d (expect 1)\n",
           atomic_load(&g_packets_dispatched));
    assert(atomic_load(&g_packets_dispatched) == 1);

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

    printf("\nTEST 5: client disconnect tears the connection down\n");
    close(c1);
    for (int i = 0; i < 400 && net_loop_connection_count() > 0; i++) usleep(5000);
    printf("  live connections after close: %d (expect 0)\n", net_loop_connection_count());
    assert(net_loop_connection_count() == 0);

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

    printf("\nTEST 7: 250 concurrent connections on a handful of threads\n");
    // exceed the worker-thread count with live connections
    enum { MANY = 250 };
    int clients[MANY];
    atomic_store(&g_packets_dispatched, 0);

    for (int i = 0; i < MANY; i++) {
        clients[i] = connect_client();
        /* One identity each: see send_auth_as(). */
        send_auth_as(clients[i], i + 1);
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

    printf("\nTEST 8: a bad ticket is rejected, not admitted\n");
    int c3 = connect_client();
    {
        WorldConnectPacket pkt;
        memset(&pkt, 0, sizeof(pkt));
        pkt.header.type = PACKET_WORLD_CONNECT;
        pkt.header.payload_size = htons(sizeof(pkt) - sizeof(PacketHeader));
        strncpy(pkt.game_ticket, "forged", sizeof(pkt.game_ticket) - 1);
        pkt.protocol_version = htons(PROTOCOL_VERSION);
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

    printf("\nTEST 9: a client on another protocol version is turned away\n");
    int c4 = connect_client();
    {
        // Ticket is valid; only the version is wrong. The rejection must come
        // from the version check, which runs before the ticket is even looked
        // at — otherwise a stale client gets "invalid ticket" and its player
        // goes looking for an account problem that does not exist.
        WorldConnectPacket pkt;
        memset(&pkt, 0, sizeof(pkt));
        pkt.header.type = PACKET_WORLD_CONNECT;
        pkt.header.payload_size = htons(sizeof(pkt) - sizeof(PacketHeader));
        strncpy(pkt.game_ticket, "test-ticket", sizeof(pkt.game_ticket) - 1);
        pkt.protocol_version = htons(PROTOCOL_VERSION + 1);
        assert(send(c4, &pkt, sizeof(pkt), 0) == (ssize_t)sizeof(pkt));
    }
    got = recv_timeout(c4, &ack, sizeof(ack), 2000);
    printf("  ack success=%d msg=\"%s\" (expect 0)\n",
           got > 0 ? ack.success : -1, got > 0 ? ack.welcome_message : "");
    assert(got == (ssize_t)sizeof(ack));
    assert(ack.success == 0);
    assert(strstr(ack.welcome_message, "version") != NULL);
    close(c4);
    for (int i = 0; i < 400 && net_loop_connection_count() > 0; i++) usleep(5000);
    assert(net_loop_connection_count() == 0);

    printf("\nTEST 10: a clean disconnect frees the character to reconnect\n");
    /* The ordinary case, and the baseline for TEST 11: after a socket closes,
     * nothing about that character is left claimed. */
    {
        int a = connect_client();
        send_auth(a);
        got = recv_timeout(a, &ack, sizeof(ack), 2000);
        assert(got == (ssize_t)sizeof(ack) && ack.success == 1);
        printf("  first session admitted, roster=%d\n", roster_count());
        assert(roster_count() == 1);

        close(a);
        for (int i = 0; i < 400 && net_loop_connection_count() > 0; i++) usleep(5000);
        for (int i = 0; i < 400 && roster_count() > 0; i++) usleep(5000);
        printf("  after the close: connections=%d roster=%d (expect 0 and 0)\n",
               net_loop_connection_count(), roster_count());
        assert(net_loop_connection_count() == 0);
        assert(roster_count() == 0);

        int b = connect_client();
        send_auth(b);
        got = recv_timeout(b, &ack, sizeof(ack), 2000);
        printf("  reconnect ack success=%d (expect 1)\n", got > 0 ? ack.success : -1);
        assert(got == (ssize_t)sizeof(ack) && ack.success == 1);
        assert(roster_count() == 1);

        close(b);
        for (int i = 0; i < 400 && net_loop_connection_count() > 0; i++) usleep(5000);
        for (int i = 0; i < 400 && roster_count() > 0; i++) usleep(5000);
        assert(roster_count() == 0);
    }

    printf("\nTEST 11: a reconnect while the old session is live kicks the stale one\n");
    /* The half-open TCP case: the client's socket is gone but the server never
     * saw the FIN, so the old session is still registered and still holding
     * the character. A player who reconnects must get in, and must not end up
     * with the world believing they are logged in twice.
     *
     * The order matters and is the subtle part: the new session is admitted
     * first, and only then does the old connection's teardown run. If that
     * teardown removed the character unconditionally instead of checking that
     * the descriptor still owns it, the reconnect would be admitted and then
     * immediately evicted by the corpse of the connection it replaced --
     * leaving a live socket attached to nobody. */
    {
        int old_c = connect_client();
        send_auth(old_c);
        got = recv_timeout(old_c, &ack, sizeof(ack), 2000);
        assert(got == (ssize_t)sizeof(ack) && ack.success == 1);
        assert(roster_count() == 1);
        printf("  old session live: connections=%d roster=%d\n",
               net_loop_connection_count(), roster_count());

        /* Reconnect without closing the first socket. */
        int new_c = connect_client();
        send_auth(new_c);
        got = recv_timeout(new_c, &ack, sizeof(ack), 2000);
        printf("  reconnect ack success=%d msg=\"%s\" (expect 1)\n",
               got > 0 ? ack.success : -1, got > 0 ? ack.welcome_message : "");
        assert(got == (ssize_t)sizeof(ack));
        assert(ack.success == 1);

        /* The registry shut the old descriptor down; the reactor notices and
         * reaps it, which is what brings the connection count back to one. */
        for (int i = 0; i < 600 && net_loop_connection_count() > 1; i++) usleep(5000);
        printf("  after the kick: connections=%d roster=%d (expect 1 and 1)\n",
               net_loop_connection_count(), roster_count());
        assert(net_loop_connection_count() == 1);
        assert(roster_count() == 1);

        /* And the surviving session is the new one: it still answers. */
        atomic_store(&g_packets_dispatched, 0);
        {
            PacketHeader ping = { .type = PACKET_PING, .player_id = 0, .payload_size = htons(0) };
            assert(send(new_c, &ping, sizeof(ping), 0) == (ssize_t)sizeof(ping));
        }
        wait_for(&g_packets_dispatched, 1, 2000);
        printf("  the surviving session dispatches: %d (expect 1)\n",
               atomic_load(&g_packets_dispatched));
        assert(atomic_load(&g_packets_dispatched) == 1);

        close(old_c);
        close(new_c);
        for (int i = 0; i < 600 && net_loop_connection_count() > 0; i++) usleep(5000);
        for (int i = 0; i < 600 && roster_count() > 0; i++) usleep(5000);
        printf("  after both close: connections=%d roster=%d (expect 0 and 0)\n",
               net_loop_connection_count(), roster_count());
        assert(net_loop_connection_count() == 0);
        assert(roster_count() == 0);
    }

    printf("\nTEST 12: repeated reconnects leave nothing behind\n");
    /* A flapping client. Each round admits a session and kicks the previous
     * one; what must not happen is the roster or the connection count
     * creeping, which is what a leak on either the kicked path or the
     * teardown path would look like. */
    {
        int prev = -1;
        for (int round = 0; round < 12; round++) {
            int c = connect_client();
            send_auth(c);
            got = recv_timeout(c, &ack, sizeof(ack), 2000);
            assert(got == (ssize_t)sizeof(ack) && ack.success == 1);
            if (prev >= 0) close(prev);
            prev = c;
            for (int i = 0; i < 600 && net_loop_connection_count() > 1; i++) usleep(5000);
        }
        printf("  after 12 rounds: connections=%d roster=%d (expect 1 and 1)\n",
               net_loop_connection_count(), roster_count());
        assert(net_loop_connection_count() == 1);
        assert(roster_count() == 1);

        close(prev);
        for (int i = 0; i < 600 && net_loop_connection_count() > 0; i++) usleep(5000);
        for (int i = 0; i < 600 && roster_count() > 0; i++) usleep(5000);
        printf("  and after the last close: connections=%d roster=%d\n",
               net_loop_connection_count(), roster_count());
        assert(net_loop_connection_count() == 0);
        assert(roster_count() == 0);
    }

    printf("\nTEST 13: a ticket with no terminator is terminated before use\n");
    /* The field is a fixed char[64] off the wire, and the code that consumes it
     * treats it as a C string. A client that fills all 64 bytes therefore used
     * to send the ticket lookup reading past the field, through character_id
     * and protocol_version and into whatever the reactor's buffer held next --
     * an out-of-bounds read reachable before authentication by anyone who could
     * open the port. The fixture's validate_game_ticket() counts any ticket it
     * is handed without a NUL inside the field; the count must stay zero.
     *
     * The connection is refused either way, so a rejection alone proves
     * nothing. The counter is the assertion. */
    atomic_store(&g_unterminated_tickets, 0);
    {
        int c6 = connect_client();
        WorldConnectPacket pkt;
        memset(&pkt, 0, sizeof(pkt));
        pkt.header.type = PACKET_WORLD_CONNECT;
        pkt.header.payload_size = htons(sizeof(pkt) - sizeof(PacketHeader));
        memset(pkt.game_ticket, 'A', sizeof(pkt.game_ticket));   /* no NUL at all */
        pkt.character_id     = htonl(0xAAAAAAAAu);               /* nor after it */
        pkt.protocol_version = htons(PROTOCOL_VERSION);
        assert(send(c6, &pkt, sizeof(pkt), 0) == (ssize_t)sizeof(pkt));

        got = recv_timeout(c6, &ack, sizeof(ack), 2000);
        printf("  ack success=%d (expect 0), unterminated seen=%d (expect 0)\n",
               got > 0 ? ack.success : -1, atomic_load(&g_unterminated_tickets));
        assert(got == (ssize_t)sizeof(ack));
        assert(ack.success == 0);
        assert(atomic_load(&g_unterminated_tickets) == 0);

        close(c6);
        for (int i = 0; i < 400 && net_loop_connection_count() > 0; i++) usleep(5000);
        assert(net_loop_connection_count() == 0);
    }

    net_loop_stop();
    session_registry_shutdown();
    close(g_listen_fd);

    printf("\nALL ASSERTIONS PASSED\n");
    return 0;
}
