/**
 * @file
 * Starting and stopping a reactor, and the public accessors.
 *
 * The contract is net_reactor.h; the work is split across net_reactor_conn.c,
 * net_reactor_work.c and net_reactor_loop.c, which share their shape through
 * net_reactor_internal.h.
 */

#include "net_reactor_internal.h"
#include "log.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/* --- Lifecycle ----------------------------------------------------------- */

/** Size the descriptor table from what the process may actually open. */
static int table_slots(void) {
    struct rlimit rl;
    long slots = 1024;
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY)
        slots = (long)rl.rlim_cur;
    if (slots < 1024) slots = 1024;
    if (slots > REACTOR_MAX_SLOTS) slots = REACTOR_MAX_SLOTS;
    return (int)slots;
}

NetReactor* net_reactor_start(const NetReactorConfig* config) {
    if (!config) return NULL;

    NetReactor* reactor = calloc(1, sizeof(*reactor));
    if (!reactor) return NULL;

    reactor->config       = *config;
    if (!reactor->config.name) reactor->config.name = "net";
    reactor->buffer_size  = config->buffer_size  ? config->buffer_size  : DEFAULT_BUFFER;
    reactor->user_size    = config->user_size;
    reactor->sweep_secs   = config->sweep_interval_secs > 0
                          ? config->sweep_interval_secs : DEFAULT_SWEEP_SECS;
    reactor->worker_count = config->worker_count > 0 ? config->worker_count : DEFAULT_WORKERS;
    reactor->job_cap      = config->job_queue_cap > 0 ? config->job_queue_cap : DEFAULT_JOB_QUEUE;

    pthread_mutex_init(&reactor->job_lock, NULL);
    pthread_cond_init(&reactor->job_ready, NULL);

    reactor->conn_count = table_slots();

    /* One slot per connection, always. A connection has at most one job
     * outstanding, so a queue this size can never fill -- which is the point:
     * an overflowing queue would drop teardown work, and teardown is where a
     * character gets saved. At eight bytes an entry this costs half a megabyte
     * to make a whole class of data loss unreachable. */
    if (reactor->job_cap < reactor->conn_count) reactor->job_cap = reactor->conn_count;

    reactor->conns = calloc((size_t)reactor->conn_count, sizeof(*reactor->conns));
    reactor->jobs  = calloc((size_t)reactor->job_cap, sizeof(*reactor->jobs));
    reactor->workers = calloc((size_t)reactor->worker_count, sizeof(*reactor->workers));

    if (!reactor->conns || !reactor->jobs || !reactor->workers) {
        LOG_ERROR("[%s] could not allocate the reactor", reactor->config.name);
        free(reactor->conns); free(reactor->jobs); free(reactor->workers); free(reactor);
        return NULL;
    }

    int loops = config->loop_count;
    if (loops <= 0) {
        long cores = sysconf(_SC_NPROCESSORS_ONLN);
        loops = (int)(cores > 0 ? cores : 1);
    }
    if (loops > REACTOR_MAX_LOOPS) loops = REACTOR_MAX_LOOPS;
    reactor->loop_count = loops;

    atomic_store(&reactor->running, 1);

    int started_loops = 0, started_workers = 0;

    for (int i = 0; i < reactor->loop_count; i++) {
        reactor->loops[i].id      = i;
        reactor->loops[i].reactor = reactor;
        reactor->loops[i].epfd    = epoll_create1(0);
        if (reactor->loops[i].epfd < 0) {
            LOG_ERROR("[%s] epoll_create1 failed: %s", reactor->config.name, strerror(errno));
            break;
        }
        if (pthread_create(&reactor->loops[i].thread, NULL,
                           net_reactor_loop_main, &reactor->loops[i]) != 0) {
            LOG_ERROR("[%s] could not start event loop %d", reactor->config.name, i);
            close(reactor->loops[i].epfd);
            reactor->loops[i].epfd = -1;
            break;
        }
        started_loops++;
    }

    if (started_loops == reactor->loop_count) {
        for (int i = 0; i < reactor->worker_count; i++) {
            if (pthread_create(&reactor->workers[i], NULL, net_reactor_worker_main, reactor) != 0) {
                LOG_ERROR("[%s] could not start worker %d", reactor->config.name, i);
                break;
            }
            started_workers++;
        }
    }

    if (started_loops != reactor->loop_count || started_workers != reactor->worker_count) {
        /* Partial startup is a failed startup: join what did start, then give
         * up, rather than run with fewer loops than connections are sharded to. */
        atomic_store(&reactor->running, 0);
        pthread_mutex_lock(&reactor->job_lock);
        pthread_cond_broadcast(&reactor->job_ready);
        pthread_mutex_unlock(&reactor->job_lock);

        for (int i = 0; i < started_loops; i++) {
            pthread_join(reactor->loops[i].thread, NULL);
            close(reactor->loops[i].epfd);
        }
        for (int i = 0; i < started_workers; i++)
            pthread_join(reactor->workers[i], NULL);

        free(reactor->conns); free(reactor->jobs); free(reactor->workers);
        pthread_mutex_destroy(&reactor->job_lock);
        pthread_cond_destroy(&reactor->job_ready);
        free(reactor);
        return NULL;
    }

    LOG_INFO("[%s] %d event loops + %d blocking workers, %d connection slots",
             reactor->config.name, reactor->loop_count, reactor->worker_count,
             reactor->conn_count);
    return reactor;
}

void net_reactor_stop(NetReactor* reactor) {
    if (!reactor) return;

    atomic_store(&reactor->running, 0);

    pthread_mutex_lock(&reactor->job_lock);
    pthread_cond_broadcast(&reactor->job_ready);
    pthread_mutex_unlock(&reactor->job_lock);

    for (int i = 0; i < reactor->loop_count; i++) {
        pthread_join(reactor->loops[i].thread, NULL);
        close(reactor->loops[i].epfd);
    }
    for (int i = 0; i < reactor->worker_count; i++)
        pthread_join(reactor->workers[i], NULL);

    /* Every loop and worker is joined, so plain access is safe from here.
     *
     * The survivors are retired properly rather than merely freed. on_retire is
     * documented to run "for every connection that closes, authenticated or
     * not, exactly once", and shutdown was the one path that broke that
     * promise: it freed the connection table directly, so the world server's
     * per-player save in world_on_retire() never ran for anyone still connected
     * when the process stopped. That it did no damage was luck -- playerdata_close()
     * happens to save every loaded player afterwards -- and luck that only holds
     * while that call stays in the shutdown order.
     *
     * A worker that had already retired a connection leaves it CONN_DEAD, so
     * the state is what decides whether on_retire still owes a call. Workers
     * drain the job queue before exiting (net_reactor_job_pop only stops on an
     * empty queue), so nothing can still be mid-teardown here. */
    int retired = 0;
    for (int fd = 0; fd < reactor->conn_count; fd++) {
        NetReactorConn* conn = atomic_load(&reactor->conns[fd]);
        if (!conn) continue;

        if (atomic_load(&conn->state) == CONN_LOOP) {
            if (reactor->config.on_retire) reactor->config.on_retire(conn);
            retired++;
        }
        atomic_store(&conn->state, CONN_DEAD);

        /* Same teardown the loop would have run: on_reap, transport close,
         * close(), slot release, live count, free. Duplicating it here is what
         * let the two drift apart in the first place. */
        net_reactor_reap(conn);
    }

    if (retired > 0) {
        LOG_INFO("[%s] retired %d connection(s) still open at shutdown",
                 reactor->config.name, retired);
    }

    free(reactor->conns);
    free(reactor->jobs);
    free(reactor->workers);
    pthread_mutex_destroy(&reactor->job_lock);
    pthread_cond_destroy(&reactor->job_ready);

    LOG_INFO("[%s] event loops stopped", reactor->config.name);
    free(reactor);
}

int net_reactor_connection_count(const NetReactor* reactor) {
    return reactor ? atomic_load(&((NetReactor*)reactor)->live) : 0;
}

int net_reactor_loop_count(const NetReactor* reactor) {
    return reactor ? reactor->loop_count : 0;
}

int net_reactor_job_depth(NetReactor* reactor, int* out_capacity) {
    if (out_capacity) *out_capacity = reactor ? reactor->job_cap : 0;
    if (!reactor) return 0;

    pthread_mutex_lock(&reactor->job_lock);
    int depth = reactor->job_count;
    pthread_mutex_unlock(&reactor->job_lock);
    return depth;
}

/* --- One connection ------------------------------------------------------ */

int net_reactor_conn_fd(const NetReactorConn* conn) {
    return conn ? conn->fd : -1;
}

void* net_reactor_conn_user(NetReactorConn* conn) {
    return conn ? conn->user : NULL;
}

uint8_t* net_reactor_conn_buffer(NetReactorConn* conn, size_t* out_len) {
    if (!conn) { if (out_len) *out_len = 0; return NULL; }
    if (out_len) *out_len = conn->buf_len;
    return conn->buf;
}

void net_reactor_conn_consume(NetReactorConn* conn, size_t bytes) {
    if (!conn) return;
    if (bytes > conn->buf_len) bytes = conn->buf_len;
    conn->buf_len -= bytes;
    if (conn->buf_len > 0) memmove(conn->buf, conn->buf + bytes, conn->buf_len);
}