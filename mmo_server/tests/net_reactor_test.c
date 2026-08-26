/**
 * @file
 * Check the generic reactor: framing, worker handoff, timeouts, and teardown.
 *
 * This is the accept path with the game taken out of it, so what is tested here
 * is only what it promises: that bytes arrive contiguously however the peer
 * chose to split them, that a connection is owned by exactly one thread at a
 * time as it moves between its loop and a worker, that everything which opens
 * gets retired and reaped exactly once, and that nothing is left running after
 * stop.
 *
 * There is no protocol anywhere in here on purpose. The owner in these tests
 * frames on a one-byte length prefix, which is enough to prove the reassembly
 * contract and nothing more.
 */

#include "net_reactor.h"
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

/* --- Counters the callbacks report through ------------------------------- */

static atomic_int g_accepted, g_messages, g_worked, g_retired, g_reaped, g_idle_closed;
static atomic_int g_handshakes;

/** Per-connection state, to prove it is private and survives a handoff. */
typedef struct {
    uint32_t magic;
    int      messages_seen;
    int      worked;
} OwnerState;

#define OWNER_MAGIC 0xC0FFEEu

static void counters_reset(void) {
    atomic_store(&g_accepted, 0);
    atomic_store(&g_messages, 0);
    atomic_store(&g_worked, 0);
    atomic_store(&g_retired, 0);
    atomic_store(&g_reaped, 0);
    atomic_store(&g_idle_closed, 0);
    atomic_store(&g_handshakes, 0);
}

/* --- Test scaffolding ---------------------------------------------------- */

static int g_listen_fd = -1;
static int g_listen_port = 0;

static void start_listener(void) {
    g_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(g_listen_fd >= 0);
    int opt = 1;
    setsockopt(g_listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    assert(bind(g_listen_fd, (struct sockaddr*)&addr, sizeof(addr)) == 0);
    assert(listen(g_listen_fd, 512) == 0);

    socklen_t len = sizeof(addr);
    assert(getsockname(g_listen_fd, (struct sockaddr*)&addr, &len) == 0);
    g_listen_port = ntohs(addr.sin_port);
}

/** Connect a loopback client and give its accepted peer to the reactor. */
static int connect_client(NetReactor* reactor) {
    int c = socket(AF_INET, SOCK_STREAM, 0);
    assert(c >= 0);

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((uint16_t)g_listen_port);
    assert(connect(c, (struct sockaddr*)&addr, sizeof(addr)) == 0);

    int peer = accept(g_listen_fd, NULL, NULL);
    assert(peer >= 0);
    net_reactor_submit(reactor, peer);
    return c;
}

/** Wait for a counter to reach a value, or give up. */
static int wait_for(atomic_int* counter, int target, int millis) {
    for (int i = 0; i < millis; i++) {
        if (atomic_load(counter) >= target) return 1;
        usleep(1000);
    }
    return 0;
}

/** Wait for the reactor to report no live connections.
 *
 * Separate from waiting on the reap counter, because the two are deliberately
 * not simultaneous: on_reap runs while the connection is still counted, so that
 * the count only falls once the teardown really is finished.
 */
static int wait_for_empty(NetReactor* reactor, int millis) {
    for (int i = 0; i < millis; i++) {
        if (net_reactor_connection_count(reactor) == 0) return 1;
        usleep(1000);
    }
    return 0;
}

/** Atomic because the callbacks below assert from loop and worker threads. */
static atomic_int passed;

static void check(int condition, const char* what) {
    if (!condition) {
        printf("  FAIL: %s\n", what);
        exit(1);
    }
    atomic_fetch_add(&passed, 1);
}

/* --- The owner ----------------------------------------------------------- */

static int on_accept(NetReactorConn* conn) {
    OwnerState* state = net_reactor_conn_user(conn);
    check(state != NULL, "a connection has owner state");
    check(state->magic == 0, "owner state arrives zeroed");
    state->magic = OWNER_MAGIC;
    atomic_fetch_add(&g_accepted, 1);
    return 0;
}

/** Frame on a one-byte length prefix; byte 'W' asks for worker handoff. */
static NetReactorVerdict on_data(NetReactorConn* conn, uint8_t* data, size_t len,
                                 size_t* consumed) {
    OwnerState* state = net_reactor_conn_user(conn);
    check(state->magic == OWNER_MAGIC, "owner state survived to on_data");

    size_t used = 0;
    while (len - used >= 1) {
        uint8_t body = data[used];
        if (len - used < 1u + body) break;      /* incomplete, carry it over */

        uint8_t verb = body ? data[used + 1] : 0;
        used += 1u + body;

        if (verb == 'X') { *consumed = used; return NET_REACTOR_CLOSE; }
        if (verb == 'W') { *consumed = used; return NET_REACTOR_TO_WORKER; }

        state->messages_seen++;
        atomic_fetch_add(&g_messages, 1);
    }

    *consumed = used;
    return NET_REACTOR_KEEP;
}

static NetReactorVerdict on_work(NetReactorConn* conn) {
    OwnerState* state = net_reactor_conn_user(conn);
    check(state->magic == OWNER_MAGIC, "owner state survived the handoff to a worker");
    state->worked++;
    atomic_fetch_add(&g_worked, 1);
    return NET_REACTOR_KEEP;
}

static void on_retire(NetReactorConn* conn) {
    OwnerState* state = net_reactor_conn_user(conn);
    check(state->magic == OWNER_MAGIC, "owner state is still there at retire");
    atomic_fetch_add(&g_retired, 1);
}

static void on_reap(NetReactorConn* conn) {
    (void)conn;
    atomic_fetch_add(&g_reaped, 1);
}

static int on_idle(NetReactorConn* conn, long idle_seconds) {
    (void)conn;
    if (idle_seconds >= 1) {
        atomic_fetch_add(&g_idle_closed, 1);
        return 1;
    }
    return 0;
}

static NetReactorConfig base_config(void) {
    NetReactorConfig config;
    memset(&config, 0, sizeof(config));
    config.name        = "test";
    config.loop_count  = 2;
    config.worker_count = 4;
    config.user_size   = sizeof(OwnerState);
    config.buffer_size = 512;
    config.on_accept   = on_accept;
    config.on_data     = on_data;
    config.on_work     = on_work;
    config.on_retire   = on_retire;
    config.on_reap     = on_reap;
    return config;
}

/** Write a framed message: one length byte, then the body. */
static void send_message(int fd, const char* body) {
    uint8_t length = (uint8_t)strlen(body);
    assert(write(fd, &length, 1) == 1);
    if (length) assert(write(fd, body, length) == (ssize_t)length);
}

/* --- Tests --------------------------------------------------------------- */

static void test_a_connection_runs_and_retires_once(void) {
    printf("a connection is accepted, carries messages, and retires exactly once\n");
    counters_reset();

    NetReactorConfig config = base_config();
    NetReactor* reactor = net_reactor_start(&config);
    check(reactor != NULL, "the reactor started");
    check(net_reactor_loop_count(reactor) == 2, "it runs the loops it was asked for");

    int client = connect_client(reactor);
    check(wait_for(&g_accepted, 1, 2000), "the connection was accepted");
    check(net_reactor_connection_count(reactor) == 1, "and is counted");

    send_message(client, "hello");
    send_message(client, "again");
    check(wait_for(&g_messages, 2, 2000), "both messages arrived");

    close(client);
    check(wait_for(&g_retired, 1, 2000), "a closed peer retires the connection");
    check(wait_for(&g_reaped, 1, 2000), "and it is reaped");
    check(atomic_load(&g_retired) == 1, "retire ran exactly once");
    check(atomic_load(&g_reaped) == 1, "reap ran exactly once");

    check(atomic_load(&g_accepted) == 1, "nothing else was accepted");
    check(wait_for_empty(reactor, 2000), "and none are left open");

    /* Nothing may touch the reactor after this: stop frees it. */
    net_reactor_stop(reactor);
}

static void test_framing_is_independent_of_writes(void) {
    printf("a message split across writes arrives whole, and a partial one waits\n");
    counters_reset();

    NetReactorConfig config = base_config();
    NetReactor* reactor = net_reactor_start(&config);
    check(reactor != NULL, "the reactor started");

    int client = connect_client(reactor);
    check(wait_for(&g_accepted, 1, 2000), "connected");

    /* One message, one byte at a time, with the loop given every chance to see
     * each fragment on its own. */
    const char* body = "fragmented";
    uint8_t length = (uint8_t)strlen(body);
    assert(write(client, &length, 1) == 1);
    for (size_t i = 0; i < strlen(body); i++) {
        assert(write(client, body + i, 1) == 1);
        usleep(2000);
    }
    check(wait_for(&g_messages, 1, 2000), "the fragments were reassembled into one message");

    /* Two messages in one write must both be seen. */
    uint8_t batch[16];
    size_t n = 0;
    batch[n++] = 2; batch[n++] = 'a'; batch[n++] = 'b';
    batch[n++] = 3; batch[n++] = 'c'; batch[n++] = 'd'; batch[n++] = 'e';
    assert(write(client, batch, n) == (ssize_t)n);
    check(wait_for(&g_messages, 3, 2000), "a coalesced write yields both messages");

    /* A prefix with no body must not be mistaken for a message. */
    uint8_t dangling[2] = { 9, 'z' };
    assert(write(client, dangling, 2) == 2);
    usleep(200000);
    check(atomic_load(&g_messages) == 3, "an incomplete message is left buffered");

    close(client);
    check(wait_for(&g_reaped, 1, 2000), "reaped");
    net_reactor_stop(reactor);
}

static void test_worker_handoff_returns_to_the_loop(void) {
    printf("blocking work runs off the loop and the connection comes back\n");
    counters_reset();

    NetReactorConfig config = base_config();
    NetReactor* reactor = net_reactor_start(&config);
    check(reactor != NULL, "the reactor started");

    int client = connect_client(reactor);
    check(wait_for(&g_accepted, 1, 2000), "connected");

    send_message(client, "W");
    check(wait_for(&g_worked, 1, 2000), "the work ran");

    /* Back on its loop: ordinary traffic flows again. */
    send_message(client, "after");
    check(wait_for(&g_messages, 1, 2000), "the connection reads again after the handoff");

    send_message(client, "W");
    check(wait_for(&g_worked, 2, 2000), "and can be handed off a second time");

    close(client);
    check(wait_for(&g_reaped, 1, 2000), "reaped");
    check(atomic_load(&g_retired) == 1, "retired once, not once per handoff");
    net_reactor_stop(reactor);
}

static void test_bytes_left_by_a_worker_are_not_stranded(void) {
    printf("data buffered behind a worker handoff is delivered when it returns\n");
    counters_reset();

    NetReactorConfig config = base_config();
    NetReactor* reactor = net_reactor_start(&config);
    check(reactor != NULL, "the reactor started");

    int client = connect_client(reactor);
    check(wait_for(&g_accepted, 1, 2000), "connected");

    /* One write carrying the handoff and a message behind it. The owner takes
     * the handoff and leaves the rest buffered.
     *
     * Nothing further will arrive on the socket, and readiness is
     * edge-triggered, so if the reactor only ever looks at the buffer after a
     * successful read this message is stranded until the peer happens to send
     * something else -- which, for a client waiting on a reply, is never. */
    uint8_t batch[16];
    size_t n = 0;
    batch[n++] = 1; batch[n++] = 'W';
    batch[n++] = 5; memcpy(batch + n, "after", 5); n += 5;
    assert(write(client, batch, n) == (ssize_t)n);

    check(wait_for(&g_worked, 1, 2000), "the handoff ran");
    check(wait_for(&g_messages, 1, 2000),
          "and the message behind it arrived without the peer sending anything more");

    close(client);
    check(wait_for(&g_reaped, 1, 2000), "reaped");
    net_reactor_stop(reactor);
}

static void test_the_owner_can_close_a_connection(void) {
    printf("an owner that says close gets a retire and a reap\n");
    counters_reset();

    NetReactorConfig config = base_config();
    NetReactor* reactor = net_reactor_start(&config);
    check(reactor != NULL, "the reactor started");

    int client = connect_client(reactor);
    check(wait_for(&g_accepted, 1, 2000), "connected");

    send_message(client, "X");
    check(wait_for(&g_retired, 1, 2000), "the connection was retired");
    check(wait_for(&g_reaped, 1, 2000), "and reaped");

    close(client);
    net_reactor_stop(reactor);
}

static void test_an_idle_connection_is_dropped(void) {
    printf("a connection the owner calls idle is closed by the sweep\n");
    counters_reset();

    NetReactorConfig config = base_config();
    config.on_idle = on_idle;
    config.sweep_interval_secs = 1;
    NetReactor* reactor = net_reactor_start(&config);
    check(reactor != NULL, "the reactor started");

    int client = connect_client(reactor);
    check(wait_for(&g_accepted, 1, 2000), "connected");

    check(wait_for(&g_idle_closed, 1, 5000), "the sweep noticed it was idle");
    check(wait_for(&g_reaped, 1, 3000), "and it was reaped");

    close(client);
    net_reactor_stop(reactor);
}

static void test_a_peer_that_never_frames_anything_is_dropped(void) {
    printf("a peer that fills the buffer without ever completing a message is closed\n");
    counters_reset();

    NetReactorConfig config = base_config();
    config.buffer_size = 128;
    NetReactor* reactor = net_reactor_start(&config);
    check(reactor != NULL, "the reactor started");

    int client = connect_client(reactor);
    check(wait_for(&g_accepted, 1, 2000), "connected");

    /* A length prefix promising more than the buffer will ever hold. Nothing
     * can be consumed, so the buffer fills and stays full. */
    uint8_t flood[256];
    memset(flood, 0xFF, sizeof(flood));
    ssize_t written = write(client, flood, sizeof(flood));
    check(written > 0, "the flood was written");

    check(wait_for(&g_reaped, 1, 3000), "the connection was dropped rather than grown");
    check(atomic_load(&g_messages) == 0, "and nothing was mistaken for a message");

    close(client);
    net_reactor_stop(reactor);
}

static void test_a_last_message_survives_the_hangup(void) {
    printf("a peer that sends and immediately closes still gets its last message read\n");
    counters_reset();

    NetReactorConfig config = base_config();
    NetReactor* reactor = net_reactor_start(&config);
    check(reactor != NULL, "the reactor started");

    int client = connect_client(reactor);
    check(wait_for(&g_accepted, 1, 2000), "the connection was accepted");

    /* Both halves in one go: the last packet, then a full close. epoll can
     * report the data and the hangup in the same event, and EPOLLHUP used to
     * close the connection before anything was read off it -- which for the
     * world server threw away the client's LOGOUT. */
    send_message(client, "farewell");
    close(client);

    check(wait_for(&g_messages, 1, 2000), "the last message was read before the close");
    check(wait_for(&g_retired, 1, 2000), "and the connection was then retired");
    check(wait_for_empty(reactor, 2000), "leaving nothing open");

    net_reactor_stop(reactor);
}

static void test_many_connections_at_once(void) {
    printf("many connections open, talk, hand off work, and all retire\n");
    counters_reset();

    NetReactorConfig config = base_config();
    NetReactor* reactor = net_reactor_start(&config);
    check(reactor != NULL, "the reactor started");

    enum { CLIENTS = 64 };
    int clients[CLIENTS];
    for (int i = 0; i < CLIENTS; i++) clients[i] = connect_client(reactor);

    check(wait_for(&g_accepted, CLIENTS, 5000), "every connection was accepted");
    check(net_reactor_connection_count(reactor) == CLIENTS, "and all are counted");

    for (int i = 0; i < CLIENTS; i++) {
        send_message(clients[i], "hello");
        send_message(clients[i], "W");
    }
    check(wait_for(&g_messages, CLIENTS, 5000), "every message arrived");
    check(wait_for(&g_worked, CLIENTS, 5000), "every handoff ran");

    for (int i = 0; i < CLIENTS; i++) close(clients[i]);
    check(wait_for(&g_retired, CLIENTS, 5000), "every connection retired");
    check(wait_for(&g_reaped, CLIENTS, 5000), "every connection was reaped");
    check(atomic_load(&g_retired) == CLIENTS, "exactly once each");
    check(wait_for_empty(reactor, 5000), "and none are left open");

    net_reactor_stop(reactor);
}

/* --- Transports ---------------------------------------------------------- */

/** A transport that refuses to finish its handshake for the first few events. */
static int slow_handshake(NetReactorConn* conn) {
    (void)conn;
    return atomic_fetch_add(&g_handshakes, 1) >= 2 ? 1 : 0;
}

static ssize_t plain_recv(NetReactorConn* conn, void* buf, size_t len) {
    return recv(net_reactor_conn_fd(conn), buf, len, 0);
}

static void test_a_handshake_gates_the_data(void) {
    printf("no data reaches the owner until the transport says the peer is ready\n");
    counters_reset();

    static const NetReactorTransport slow = {
        .handshake = slow_handshake,
        .recv      = plain_recv,
        .close     = NULL,
    };

    NetReactorConfig config = base_config();
    config.transport = &slow;
    config.loop_count = 1;
    NetReactor* reactor = net_reactor_start(&config);
    check(reactor != NULL, "the reactor started");

    int client = connect_client(reactor);
    check(wait_for(&g_accepted, 1, 2000), "connected");

    send_message(client, "early");

    /* The handshake needs more events than one write provides, so more writes
     * are what supplies them. */
    for (int i = 0; i < 6 && atomic_load(&g_messages) == 0; i++) {
        send_message(client, "again");
        usleep(20000);
    }

    check(atomic_load(&g_handshakes) >= 3, "the handshake was driven across events");
    check(wait_for(&g_messages, 1, 2000), "and the buffered data arrived once it finished");

    close(client);
    check(wait_for(&g_reaped, 1, 2000), "reaped");
    net_reactor_stop(reactor);
}

static void test_stop_is_safe_with_connections_open(void) {
    printf("stopping with live connections joins everything and frees it\n");
    counters_reset();

    NetReactorConfig config = base_config();
    NetReactor* reactor = net_reactor_start(&config);
    check(reactor != NULL, "the reactor started");

    int clients[8];
    for (int i = 0; i < 8; i++) clients[i] = connect_client(reactor);
    check(wait_for(&g_accepted, 8, 3000), "all connected");

    /* Two assertions. That this returns at all, and that the sanitizers see a
     * clean teardown afterwards -- every loop and worker joined, every
     * connection freed -- which `make net-reactor-sanitize` is what actually
     * checks. And that on_retire keeps its promise here: it is documented to
     * run exactly once for every connection that closes, and shutdown used to
     * be the one path that freed connections without it. For the world server
     * that callback is the character's save. */
    net_reactor_stop(reactor);

    check(atomic_load(&g_accepted) == 8, "all eight had been accepted before the stop");
    check(atomic_load(&g_retired) == 8, "every open connection was retired by the stop");
    check(atomic_load(&g_reaped) == 8, "and reaped, exactly once each");

    for (int i = 0; i < 8; i++) close(clients[i]);
}

static void test_a_descriptor_past_the_table_is_refused(void) {
    printf("a descriptor the table cannot hold is refused, not written past\n");
    counters_reset();

    NetReactorConfig config = base_config();
    NetReactor* reactor = net_reactor_start(&config);
    check(reactor != NULL, "the reactor started");

    net_reactor_submit(reactor, 1 << 28);
    net_reactor_submit(reactor, -1);
    usleep(100000);
    check(net_reactor_connection_count(reactor) == 0, "neither was taken");
    check(atomic_load(&g_accepted) == 0, "and neither was accepted");

    net_reactor_stop(reactor);
}

int main(void) {
    printf("=== net reactor ===\n");
    log_set_level(LOG_LEVEL_ERROR);
    start_listener();

    test_a_connection_runs_and_retires_once();
    test_framing_is_independent_of_writes();
    test_worker_handoff_returns_to_the_loop();
    test_bytes_left_by_a_worker_are_not_stranded();
    test_the_owner_can_close_a_connection();
    test_an_idle_connection_is_dropped();
    test_a_peer_that_never_frames_anything_is_dropped();
    test_a_last_message_survives_the_hangup();
    test_many_connections_at_once();
    test_a_handshake_gates_the_data();
    test_stop_is_safe_with_connections_open();
    test_a_descriptor_past_the_table_is_refused();

    close(g_listen_fd);
    printf("\n%d checks passed\n", atomic_load(&passed));
    return 0;
}
