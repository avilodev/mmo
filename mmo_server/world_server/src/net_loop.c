// ============================================================================
// net_loop.c — epoll event loops plus a small blocking worker pool.
//
// See net_loop.h for the ownership model. The short version: a connection is
// owned by its loop while it is in that loop's epoll set, and by a worker while
// it is not. Handoff always goes through epoll_ctl(DEL) first, so the two never
// touch a connection at the same time.
//
// Sockets are non-blocking and registered edge-triggered. Edge-triggered writes
// are safe here because connection_io only ever queues after send() returned
// EAGAIN — that is, after the socket buffer filled — so a writable transition
// is guaranteed to follow and re-arm the flush.
// ============================================================================

#include "net_loop.h"

#include "ability_handler.h"
#include "config.h"
#include "connection_io.h"
#include "limit_profiles.h"
#include "log.h"
#include "net_notify.h"
#include "packet_limiter.h"
#include "party.h"
#include "player_data.h"
#include "player_level.h"
#include "players_database.h"
#include "protocol.h"
#include "quest_system.h"
#include "realm_world_auth.h"
#include "routes.h"
#include "session_registry.h"
#include "types.h"
#include "utils.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

// ---------------------------------------------------------------------------
// Tunables
// ---------------------------------------------------------------------------

#define NET_LOOP_MAX_LOOPS     8
#define NET_LOOP_WORKERS      32     // blocking pool: bounds login/logout rate
#define NET_LOOP_EVENTS      256     // events pulled per epoll_wait
#define NET_LOOP_TICK_MS    1000     // how often idle sweeps run

#define AUTH_TIMEOUT_SECS     15     // silence allowed before a client authenticates
#define PING_TIMEOUT_SECS     35     // ~3 missed 10s pings on an active session

#define JOB_QUEUE_CAP       4096

// Upper bound on tracked descriptors. Matches the packet limiter and
// connection_io so a connection is either fully tracked by all three or
// refused outright -- never half-tracked.
#define NET_LOOP_MAX_SLOTS  65536

// ---------------------------------------------------------------------------
// Connection state
// ---------------------------------------------------------------------------

typedef enum {
    CONN_AUTH_READING = 1,   // loop-owned: waiting for a full WorldConnectPacket
    CONN_AUTH_WORKING,       // worker-owned: blocking ticket/DB work
    CONN_ACTIVE,             // loop-owned: normal packet flow
    CONN_CLEANUP,            // worker-owned: blocking save
    CONN_DEAD                // worker finished; the owning loop reaps it
} ConnState;

typedef struct {
    int          fd;
    int          loop;
    _Atomic int  state;
    uint32_t     account_id;
    uint32_t     character_id;
    _Atomic long last_recv;          // seconds, CLOCK_MONOTONIC
    ssize_t      buf_len;
    uint8_t      buf[MAX_PACKET_SIZE * 2];
} Connection;

// Indexed by fd. Pointers rather than inline structs: each Connection carries a
// 16KB reassembly buffer, so a table sized to the descriptor limit would cost
// gigabytes if it held them directly.
//
// The slots are atomic because the idle sweep walks the table while other
// threads are publishing and retiring entries. Atomicity alone would not be
// enough to make that safe, so it is paired with a strict rule:
//
//   ONLY THE OWNING LOOP CLEARS A SLOT, FREES A CONNECTION, OR CLOSES ITS FD.
//
// A worker that finishes its blocking work marks the connection CONN_DEAD and
// stops there. The loop reaps it on the next sweep. That ordering matters for
// more than the pointer: if a worker closed the descriptor itself, the kernel
// could hand the same number to a fresh accept() before the loop had cleared
// the slot, and the new connection would land on top of the old one.
static _Atomic(Connection*)* g_conns = NULL;
static int          g_conn_count = 0;   // table size, not live connections
static atomic_int   g_live       = 0;

typedef struct {
    int       epfd;
    int       id;
    pthread_t thread;
} NetLoop;

static NetLoop  g_loops[NET_LOOP_MAX_LOOPS];
static int      g_loop_count = 0;
static atomic_int g_running  = 0;

// ---------------------------------------------------------------------------
// Blocking job queue
// ---------------------------------------------------------------------------

typedef enum { JOB_AUTH = 1, JOB_CLEANUP } JobType;

typedef struct {
    int     fd;
    JobType type;
} Job;

static Job             g_jobs[JOB_QUEUE_CAP];
static int             g_job_head = 0, g_job_tail = 0, g_job_count = 0;
static pthread_mutex_t g_job_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_job_ready = PTHREAD_COND_INITIALIZER;
static pthread_t       g_workers[NET_LOOP_WORKERS];

static long mono_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec;
}

static int job_push(int fd, JobType type) {
    pthread_mutex_lock(&g_job_lock);
    if (g_job_count >= JOB_QUEUE_CAP) {
        pthread_mutex_unlock(&g_job_lock);
        LOG_ERROR("[NET] job queue full, dropping %s for fd %d",
                  type == JOB_AUTH ? "auth" : "cleanup", fd);
        return 0;
    }
    g_jobs[g_job_tail] = (Job){ .fd = fd, .type = type };
    g_job_tail = (g_job_tail + 1) % JOB_QUEUE_CAP;
    g_job_count++;
    pthread_cond_signal(&g_job_ready);
    pthread_mutex_unlock(&g_job_lock);
    return 1;
}

// Returns 0 with job->fd == -1 when the pool is shutting down.
static int job_pop(Job* out) {
    pthread_mutex_lock(&g_job_lock);
    while (g_job_count == 0) {
        if (!atomic_load(&g_running)) {
            pthread_mutex_unlock(&g_job_lock);
            return 0;
        }
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 1;
        pthread_cond_timedwait(&g_job_ready, &g_job_lock, &ts);
    }
    *out = g_jobs[g_job_head];
    g_job_head = (g_job_head + 1) % JOB_QUEUE_CAP;
    g_job_count--;
    pthread_mutex_unlock(&g_job_lock);
    return 1;
}

// ---------------------------------------------------------------------------
// Connection lifecycle
// ---------------------------------------------------------------------------

static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static Connection* conn_for(int fd) {
    if (!g_conns || fd < 0 || fd >= g_conn_count) return NULL;
    return atomic_load_explicit(&g_conns[fd], memory_order_acquire);
}

// Detach from epoll so no loop can touch this connection, then hand it to the
// worker pool. Must be called from the owning loop.
static void hand_to_worker(Connection* conn, ConnState next, JobType job) {
    epoll_ctl(g_loops[conn->loop].epfd, EPOLL_CTL_DEL, conn->fd, NULL);
    atomic_store(&conn->state, next);
    if (!job_push(conn->fd, job)) {
        // Queue overflow. Retire it without the blocking work rather than
        // leaking the connection; the loop still does the actual reaping.
        atomic_store(&conn->state, CONN_DEAD);
    }
}

// Re-arm a connection on its loop. Called by a worker once its blocking work is
// done and ownership returns to the loop.
static int arm_on_loop(Connection* conn) {
    struct epoll_event ev = {0};
    ev.events  = EPOLLIN | EPOLLOUT | EPOLLET | EPOLLRDHUP;
    ev.data.fd = conn->fd;
    if (epoll_ctl(g_loops[conn->loop].epfd, EPOLL_CTL_ADD, conn->fd, &ev) != 0) {
        LOG_ERROR("[NET] epoll_ctl ADD failed for fd %d: %s",
                  conn->fd, strerror(errno));
        return 0;
    }
    return 1;
}

void net_loop_submit(int fd) {
    if (fd < 0) return;

    if (!g_conns || fd >= g_conn_count) {
        LOG_ERROR("[NET] fd %d outside connection table (%d) — refusing",
                  fd, g_conn_count);
        close(fd);
        return;
    }
    if (set_nonblocking(fd) != 0) {
        LOG_ERROR("[NET] could not set fd %d non-blocking: %s", fd, strerror(errno));
        close(fd);
        return;
    }

    Connection* conn = calloc(1, sizeof(Connection));
    if (!conn) {
        LOG_ERROR("[NET] out of memory accepting fd %d", fd);
        close(fd);
        return;
    }

    conn->fd   = fd;
    conn->loop = fd % g_loop_count;   // pinned for life; never migrated
    conn->buf_len = 0;
    atomic_store(&conn->state, CONN_AUTH_READING);
    atomic_store(&conn->last_recv, mono_seconds());

    // The budget starts here, before the first byte is read, so a client that
    // floods during authentication is metered like any other.
    packet_limiter_reset(fd);

    // Release store: a loop that sees this pointer must also see a fully
    // initialized Connection behind it.
    atomic_store_explicit(&g_conns[fd], conn, memory_order_release);
    atomic_fetch_add(&g_live, 1);

    if (!arm_on_loop(conn)) {
        // Never reached the loop, so nothing else can be looking at it yet.
        atomic_store_explicit(&g_conns[fd], NULL, memory_order_release);
        atomic_fetch_sub(&g_live, 1);
        packet_limiter_reset(fd);
        close(fd);
        free(conn);
    }
}

int net_loop_connection_count(void) {
    return atomic_load(&g_live);
}

// ---------------------------------------------------------------------------
// Worker: session start
// ---------------------------------------------------------------------------

// Tell the client why it is not getting in, then retire the connection. The
// socket stays open until the owning loop reaps it — see the g_conns comment.
static void reject_connection(Connection* conn, const char* message) {
    WorldConnectAckPacket response = {0};
    response.header.type = PACKET_WORLD_CONNECT_ACK;
    response.header.payload_size = htons(sizeof(response) - sizeof(PacketHeader));
    response.success = 0;
    strncpy(response.welcome_message, message, sizeof(response.welcome_message) - 1);
    server_send_direct(conn->fd, &response, sizeof(response));

    atomic_store(&conn->state, CONN_DEAD);
}

// Runs on a worker thread. Everything here may block.
static void worker_authenticate(Connection* conn) {
    WorldConnectPacket* pkt = (WorldConnectPacket*)conn->buf;

    if (pkt->header.type != PACKET_WORLD_CONNECT) {
        printf("Invalid authentication attempt from fd %d\n", conn->fd);
        reject_connection(conn, "Invalid ticket");
        return;
    }

    uint32_t account_id = 0, character_id = 0, world_id = 0;
    if (!validate_game_ticket(pkt->game_ticket, &account_id, &character_id, &world_id)) {
        reject_connection(conn, "Invalid ticket");
        return;
    }

    // Verify ownership before touching the registry.
    uint32_t owner = character_get_owner(character_id);
    if (owner != account_id) {
        printf("Character %u doesn't belong to account %u (owner=%u)!\n",
               character_id, account_id, owner);
        reject_connection(conn, "Character ownership verification failed");
        return;
    }

    if (session_registry_add(conn->fd, account_id, character_id) != 0) {
        printf("Account %u already logged in\n", account_id);
        reject_connection(conn, "Account already logged in");
        return;
    }

    if (!player_add_active(character_id, conn->fd)) {
        session_registry_remove(conn->fd);
        reject_connection(conn, "Invalid ticket");
        return;
    }

    if (!connection_io_register(conn->fd)) {
        session_registry_remove(conn->fd);
        player_remove_active_if_fd(character_id, conn->fd);
        reject_connection(conn, "Server busy");
        return;
    }

    conn->account_id   = account_id;
    conn->character_id = character_id;
    g_state.current_players++;

    WorldConnectAckPacket response = {0};
    response.header.type = PACKET_WORLD_CONNECT_ACK;
    response.header.player_id = htonl(character_id);
    response.header.payload_size = htons(sizeof(response) - sizeof(PacketHeader));
    response.success = 1;
    strncpy(response.welcome_message, "Welcome to the world!", 127);

    server_send(conn->fd, &response, sizeof(response));
    player_send_data_response(conn->fd, character_id);

    ActivePlayer* p = player_acquire(character_id);
    if (p) {
        player_send_stats_locked(conn->fd, p);
        ability_send_data(conn->fd, p);
        p->is_ready = 1;   // handshake complete, broadcasts may start
        player_release(p);
    }
    quest_send_all(character_id, conn->fd);

    printf("Account %u, Character %u entered world\n", account_id, character_id);

    // Preserve any bytes the client coalesced after the auth packet.
    conn->buf_len -= (ssize_t)sizeof(WorldConnectPacket);
    if (conn->buf_len > 0)
        memmove(conn->buf, conn->buf + sizeof(WorldConnectPacket), (size_t)conn->buf_len);
    else
        conn->buf_len = 0;

    atomic_store(&conn->last_recv, mono_seconds());
    atomic_store(&conn->state, CONN_ACTIVE);

    // Ownership returns to the loop only once the state is ACTIVE, so the loop
    // can never observe a half-initialized session.
    if (!arm_on_loop(conn)) {
        atomic_store(&conn->state, CONN_CLEANUP);
        if (!job_push(conn->fd, JOB_CLEANUP))
            atomic_store(&conn->state, CONN_DEAD);
    }
}

// ---------------------------------------------------------------------------
// Worker: session end
// ---------------------------------------------------------------------------

// Lifted verbatim in behaviour from the old client_handler_thread cleanup path.
static void worker_cleanup(Connection* conn) {
    int      client_fd    = conn->fd;
    uint32_t character_id = conn->character_id;
    uint32_t account_id   = conn->account_id;
    int      authenticated = (character_id != 0);

    printf("[CLEANUP] fd=%d: cleanup starting (authenticated=%d, char=%u, account=%u)\n",
           client_fd, authenticated, character_id, account_id);

    if (authenticated) {
        // Remove from the registry FIRST so a fast reconnect isn't blocked
        // while the (potentially slow) database save runs below.
        session_registry_remove(client_fd);
        g_state.current_players--;

        // Only save/remove if WE still own the active player slot. When a stale
        // session is kicked, the new connection may already have loaded the same
        // character by the time we get here.
        ActivePlayer* player = player_acquire(character_id);
        int do_save = 0;
        CharacterInfo save_data;
        PlayerQuestEntry quest_save[MAX_PLAYER_QUESTS];
        int quest_save_count = 0;

        if (player) {
            if (player->client_fd == client_fd) {
                if (player->is_loaded) {
                    // Copy out under the slot lock, write to the database after
                    // releasing it, so broadcast threads aren't blocked for the
                    // full duration of the write (~1-2s).
                    memset(&save_data, 0, sizeof(save_data));
                    save_data.character_id  = player->character_id;
                    save_data.level         = player->level;
                    save_data.pos_x         = player->pos_x;
                    save_data.pos_y         = player->pos_y;
                    save_data.health        = player->health;
                    save_data.max_health    = player->max_health;
                    save_data.mana          = player->mana;
                    save_data.max_mana      = player->max_mana;
                    save_data.experience    = player->experience;
                    save_data.gold          = player->gold;
                    save_data.helmet        = player->helmet;
                    save_data.gloves        = player->gloves;
                    save_data.chest_armor   = player->chest_armor;
                    save_data.leggings      = player->leggings;
                    save_data.boots         = player->boots;
                    save_data.main_hand     = player->main_hand;
                    save_data.second_hand   = player->second_hand;
                    save_data.blessing      = player->blessing;
                    memcpy(save_data.inventory, player->inventory, sizeof(save_data.inventory));
                    quest_save_count = player->quest_count;
                    memcpy(quest_save, player->quests,
                           (size_t)quest_save_count * sizeof(*quest_save));
                    do_save = 1;
                    // Clear dirty so player_remove_active won't repeat the write
                    // while holding active_players_lock.
                    player->is_dirty = 0;
                }
                player_release(player);
                if (player_remove_active_if_fd(character_id, client_fd)) {
                    party_handle_disconnect(character_id);
                } else {
                    // A reconnect rebound the slot after our snapshot. Do not
                    // save stale data over the live session.
                    do_save = 0;
                    printf("[CLEANUP] fd=%d: slot rebound during cleanup — preserving new session\n",
                           client_fd);
                }
            } else {
                printf("[CLEANUP] fd=%d: slot now owned by fd=%d (char=%u) — skipping save\n",
                       client_fd, player->client_fd, character_id);
                player_release(player);
            }
        } else {
            printf("[CLEANUP] fd=%d: player slot not found for char=%u (already removed?)\n",
                   client_fd, character_id);
        }

        if (do_save) {
            if (!character_update_full_data(&save_data))
                fprintf(stderr, "[CLEANUP] failed to save character %u\n", character_id);
            if (!quest_player_save(character_id, quest_save, quest_save_count))
                fprintf(stderr, "[CLEANUP] failed to save quests for character %u\n", character_id);
        }
        printf("Account %u, Character %u disconnected (fd=%d)\n",
               account_id, character_id, client_fd);
    }

    // Last chance to push a queued disconnect reason out before the socket goes
    // away, so the client can say why rather than inferring it from a timeout.
    connection_io_flush(client_fd);

    // Unregistering, closing and freeing all belong to the owning loop. Doing
    // any of it here would race the idle sweep and could hand a live descriptor
    // number back to accept() while its slot was still occupied.
    atomic_store(&conn->state, CONN_DEAD);
}

static void* worker_thread(void* arg) {
    (void)arg;
    Job job;
    while (job_pop(&job)) {
        Connection* conn = conn_for(job.fd);
        if (!conn) continue;
        if (job.type == JOB_AUTH) worker_authenticate(conn);
        else                      worker_cleanup(conn);
    }
    return NULL;
}

// ---------------------------------------------------------------------------
// Loop: reading and dispatch
// ---------------------------------------------------------------------------

// Drain every complete packet sitting in the reassembly buffer.
// Returns 0 to keep the connection, -1 to close it.
static int dispatch_packets(Connection* conn) {
    uint8_t* ptr = conn->buf;
    ssize_t remaining = conn->buf_len;

    while (remaining >= (ssize_t)sizeof(PacketHeader)) {
        PacketHeader* header = (PacketHeader*)ptr;
        size_t packet_size = sizeof(PacketHeader) + ntohs(header->payload_size);

        if (packet_size > MAX_PACKET_SIZE) {
            printf("[ERROR] Client %u sent oversized packet (type=%d, size=%zu) — disconnecting\n",
                   conn->character_id, header->type, packet_size);
            uint8_t dc[sizeof(DisconnectPacket)];
            size_t dn = net_build_disconnect(dc, sizeof(dc), DISCONNECT_REASON_PROTOCOL, NULL);
            if (dn) connection_io_send(conn->fd, dc, dn);
            return -1;
        }

        if (remaining < (ssize_t)packet_size) break;   // incomplete, carry over

        session_update_activity(conn->fd);
        int result = process_packet(conn->fd, conn->character_id, (ssize_t)packet_size, ptr);

        ptr       += packet_size;
        remaining -= packet_size;

        if (result == -1) {
            // Clean logout, or the limiter asked for the socket to close.
            conn->buf_len = 0;
            return -1;
        }
    }

    if (remaining > 0 && ptr != conn->buf)
        memmove(conn->buf, ptr, (size_t)remaining);
    conn->buf_len = remaining;
    return 0;
}

// Read until EAGAIN. Edge-triggered epoll only reports the transition, so
// stopping early would leave bytes unread until the next one.
// Returns 0 to keep the connection, -1 to close it, 1 if it was handed off.
static int loop_read(Connection* conn) {
    while (1) {
        size_t space = sizeof(conn->buf) - (size_t)conn->buf_len;
        if (space == 0) {
            // Buffer full without a complete packet: the peer is not speaking
            // our protocol.
            printf("[NET] fd=%d reassembly buffer full — disconnecting\n", conn->fd);
            return -1;
        }

        ssize_t bytes = recv(conn->fd, conn->buf + conn->buf_len, space, 0);
        if (bytes > 0) {
            conn->buf_len += bytes;
            atomic_store(&conn->last_recv, mono_seconds());

            if (atomic_load(&conn->state) == CONN_AUTH_READING) {
                if (conn->buf_len >= (ssize_t)sizeof(WorldConnectPacket)) {
                    // Ticket validation and the character load both block, so
                    // the connection leaves the loop here.
                    hand_to_worker(conn, CONN_AUTH_WORKING, JOB_AUTH);
                    return 1;
                }
                continue;   // wait for the rest of the auth packet
            }

            if (dispatch_packets(conn) != 0) return -1;
            continue;
        }

        if (bytes == 0) return -1;                       // peer closed
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;   // drained
        return -1;                                        // real error
    }
}

// Close a loop-owned connection by handing it to a worker for the blocking
// save. Unauthenticated connections have nothing to save but still go through
// the pool so there is exactly one teardown path.
static void loop_close(Connection* conn) {
    hand_to_worker(conn, CONN_CLEANUP, JOB_CLEANUP);
}

// Retire a connection a worker has finished with. Only ever called from the
// owning loop, which is what makes closing the descriptor here safe: the number
// cannot be recycled by accept() until after the slot is cleared.
static void reap(Connection* conn) {
    int fd = conn->fd;

    connection_io_unregister(fd);
    packet_limiter_reset(fd);      // don't let a recycled fd inherit the budget
    close(fd);

    atomic_store_explicit(&g_conns[fd], NULL, memory_order_release);
    atomic_fetch_sub(&g_live, 1);
    free(conn);
}

// Walk this loop's connections: reap the finished ones, close the quiet ones.
static void sweep_idle(int loop_id) {
    long now = mono_seconds();

    for (int fd = loop_id; fd < g_conn_count; fd += g_loop_count) {
        Connection* conn = atomic_load_explicit(&g_conns[fd], memory_order_acquire);
        if (!conn) continue;

        int state = atomic_load(&conn->state);

        if (state == CONN_DEAD) {
            reap(conn);
            continue;
        }

        // Everything else that is not loop-owned is mid-handoff to a worker and
        // must not be touched.
        if (state != CONN_ACTIVE && state != CONN_AUTH_READING) continue;

        long idle = now - atomic_load(&conn->last_recv);
        long limit = (state == CONN_ACTIVE) ? PING_TIMEOUT_SECS : AUTH_TIMEOUT_SECS;

        if (idle > limit) {
            if (state == CONN_ACTIVE)
                printf("[TIMEOUT] Character %u timed out (no data for %lds)\n",
                       conn->character_id, idle);
            else
                printf("[TIMEOUT] fd=%d never authenticated (%lds) — dropping\n",
                       conn->fd, idle);
            loop_close(conn);
            continue;
        }

        if (state == CONN_ACTIVE && connection_io_failed(conn->fd))
            loop_close(conn);
    }
}

static void* loop_thread(void* arg) {
    NetLoop* loop = (NetLoop*)arg;
    struct epoll_event events[NET_LOOP_EVENTS];
    long last_sweep = mono_seconds();

    LOG_INFO("[NET] event loop %d started", loop->id);

    while (atomic_load(&g_running)) {
        int n = epoll_wait(loop->epfd, events, NET_LOOP_EVENTS, NET_LOOP_TICK_MS);
        if (n < 0) {
            if (errno == EINTR) continue;
            LOG_ERROR("[NET] epoll_wait failed on loop %d: %s", loop->id, strerror(errno));
            break;
        }

        for (int i = 0; i < n; i++) {
            int fd = events[i].data.fd;
            Connection* conn = conn_for(fd);
            if (!conn) continue;

            int state = atomic_load(&conn->state);
            if (state != CONN_ACTIVE && state != CONN_AUTH_READING) continue;

            uint32_t ev = events[i].events;

            if (ev & (EPOLLERR | EPOLLHUP)) {
                loop_close(conn);
                continue;
            }

            // Flush before reading: a writable transition may be the only
            // notification we get that a queued broadcast can go out.
            if (ev & EPOLLOUT) {
                if (connection_io_flush(fd) != 0 && state == CONN_ACTIVE) {
                    loop_close(conn);
                    continue;
                }
            }

            if (ev & (EPOLLIN | EPOLLRDHUP)) {
                int r = loop_read(conn);
                if (r == -1) { loop_close(conn); continue; }
                if (r == 1)  continue;    // handed to a worker
            }
        }

        long now = mono_seconds();
        if (now - last_sweep >= 1) {
            sweep_idle(loop->id);
            last_sweep = now;
        }
    }

    LOG_INFO("[NET] event loop %d exiting", loop->id);
    return NULL;
}

// ---------------------------------------------------------------------------
// Startup / shutdown
// ---------------------------------------------------------------------------

int net_loop_start(void) {
    struct rlimit rl;
    long slots = 1024;
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY)
        slots = (long)rl.rlim_cur;
    if (slots < 1024)      slots = 1024;
    if (slots > NET_LOOP_MAX_SLOTS) slots = NET_LOOP_MAX_SLOTS;

    g_conns = calloc((size_t)slots, sizeof(*g_conns));
    if (!g_conns) {
        LOG_ERROR("[NET] could not allocate connection table (%ld slots)", slots);
        return -1;
    }
    g_conn_count = (int)slots;

    long cores = sysconf(_SC_NPROCESSORS_ONLN);
    if (cores < 1) cores = 1;
    g_loop_count = (int)(cores < NET_LOOP_MAX_LOOPS ? cores : NET_LOOP_MAX_LOOPS);

    atomic_store(&g_running, 1);

    for (int i = 0; i < g_loop_count; i++) {
        g_loops[i].id   = i;
        g_loops[i].epfd = epoll_create1(0);
        if (g_loops[i].epfd < 0) {
            LOG_ERROR("[NET] epoll_create1 failed: %s", strerror(errno));
            atomic_store(&g_running, 0);
            return -1;
        }
        if (pthread_create(&g_loops[i].thread, NULL, loop_thread, &g_loops[i]) != 0) {
            LOG_ERROR("[NET] could not start event loop %d", i);
            atomic_store(&g_running, 0);
            return -1;
        }
    }

    for (int i = 0; i < NET_LOOP_WORKERS; i++) {
        if (pthread_create(&g_workers[i], NULL, worker_thread, NULL) != 0) {
            LOG_ERROR("[NET] could not start worker %d", i);
            atomic_store(&g_running, 0);
            return -1;
        }
    }

    LOG_INFO("[NET] %d event loops + %d blocking workers, %d connection slots",
             g_loop_count, NET_LOOP_WORKERS, g_conn_count);
    return 0;
}

void net_loop_stop(void) {
    atomic_store(&g_running, 0);

    pthread_mutex_lock(&g_job_lock);
    pthread_cond_broadcast(&g_job_ready);
    pthread_mutex_unlock(&g_job_lock);

    for (int i = 0; i < g_loop_count; i++) {
        pthread_join(g_loops[i].thread, NULL);
        close(g_loops[i].epfd);
    }
    for (int i = 0; i < NET_LOOP_WORKERS; i++)
        pthread_join(g_workers[i], NULL);

    // Every loop and worker has been joined, so plain access is safe here.
    if (g_conns) {
        for (int fd = 0; fd < g_conn_count; fd++) {
            Connection* conn = atomic_load(&g_conns[fd]);
            if (!conn) continue;
            close(fd);
            free(conn);
            atomic_store(&g_conns[fd], NULL);
        }
        free(g_conns);
        g_conns = NULL;
    }
    g_conn_count = 0;
    atomic_store(&g_live, 0);
    LOG_INFO("[NET] event loops stopped");
}
