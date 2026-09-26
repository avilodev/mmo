#ifndef NET_REACTOR_INTERNAL_H
#define NET_REACTOR_INTERNAL_H

/** @file Share the reactor's shape between its four translation units.
 *
 * Not a public header and never installed: net_reactor.h is the contract, and
 * everything here is how it is met. It exists because the reactor is split by
 * what a reader is looking for --
 *
 *   net_reactor_conn.c   one connection: the table, taking one on, letting go
 *   net_reactor_work.c   the blocking pool: the queue and the worker threads
 *   net_reactor_loop.c   the event loops: readiness, reading, the idle sweep
 *   net_reactor.c        starting and stopping the whole thing
 *
 * -- and those four all need the same two structs. The alternative was one file
 * that only ever grows, which is how an event loop becomes the file nobody
 * wants to open.
 */

#include "net_reactor.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>

/** Cap the loops one reactor runs. Beyond a handful they contend, not scale. */
#define REACTOR_MAX_LOOPS      8
/** Cap the descriptor table. Matches the other per-descriptor registries. */
#define REACTOR_MAX_SLOTS  65536
/** Events pulled from one epoll_wait. */
#define REACTOR_EVENTS       256
/** How long a loop blocks in epoll_wait before checking for shutdown. */
#define REACTOR_TICK_MS     1000

#define DEFAULT_WORKERS       32
#define DEFAULT_JOB_QUEUE   4096
#define DEFAULT_BUFFER     16384
#define DEFAULT_SWEEP_SECS     1

/** Identify the current owner and lifecycle phase of a connection.
 *
 * The whole of the ownership rule is in this enum. LOOP and its sibling states
 * say "the loop may touch this"; WORKING and RETIRING say "a worker holds it,
 * do not"; DEAD says "the worker is finished, the loop may reap it".
 */
typedef enum {
    CONN_LOOP = 1,   /**< Loop-owned: reading, writing, sweeping. */
    CONN_WORKING,    /**< Worker-owned: running on_work. */
    CONN_RETIRING,   /**< Worker-owned: running on_retire. */
    CONN_DEAD        /**< Worker finished; the owning loop reaps it. */
} ConnState;

struct NetReactorConn {
    int          fd;
    int          loop;
    _Atomic int  state;
    _Atomic long last_recv;      /**< Seconds on CLOCK_MONOTONIC. */

    NetReactor*  reactor;

    /** Nonzero once the transport's handshake has finished, or immediately when
     *  there is no transport. Loop-owned. */
    int          ready;

    size_t       buf_len;
    uint8_t*     buf;            /**< buffer_size bytes. */
    void*        user;           /**< user_size bytes, or NULL when none asked for. */
};

/** Identify blocking work. */
typedef enum { JOB_WORK = 1, JOB_RETIRE } JobType;

typedef struct {
    int     fd;
    JobType type;
} Job;

/** One epoll loop and the thread running it. */
typedef struct {
    int         epfd;
    int         id;
    NetReactor* reactor;
    pthread_t   thread;
} ReactorLoop;

struct NetReactor {
    NetReactorConfig config;

    _Atomic(NetReactorConn*)* conns;   /**< Indexed by descriptor. */
    int         conn_count;            /**< Table size, not live connections. */
    atomic_int  live;

    ReactorLoop loops[REACTOR_MAX_LOOPS];
    int         loop_count;
    atomic_int  running;

    Job*            jobs;
    int             job_cap;
    int             job_head, job_tail, job_count;
    pthread_mutex_t job_lock;
    pthread_cond_t  job_ready;

    pthread_t*  workers;
    int         worker_count;

    size_t      buffer_size;
    size_t      user_size;
    int         sweep_secs;
};

/* --- Shared between the four files --------------------------------------- */

/** Seconds on CLOCK_MONOTONIC. The reactor's only notion of time. */
long net_reactor_now(void);

/** Put a descriptor into non-blocking mode. */
int net_reactor_set_nonblocking(int fd);

/** Find a live connection by descriptor, or NULL. */
NetReactorConn* net_reactor_conn_at(NetReactor* reactor, int fd);

/** Register a connection with its pinned loop.
 *
 * Both directions are always armed, edge-triggered: a handshake that needs to
 * write gets its event without the loop having to model which way it is stuck.
 *
 * @return Nonzero when registered.
 */
int net_reactor_arm(NetReactorConn* conn);

/** Detach a loop-owned connection and hand it to a worker. */
void net_reactor_hand_off(NetReactorConn* conn, ConnState next, JobType job);

/** Send a loop-owned connection down the retire path. */
void net_reactor_close_from_loop(NetReactorConn* conn);

/** Close and free a finished connection, from its owning loop. */
void net_reactor_reap(NetReactorConn* conn);

/** Release one connection's memory and transport without touching the table.
 *
 * For the shutdown path, where every loop and worker has already been joined.
 */
void net_reactor_free_conn(NetReactorConn* conn);

/* Queue --------------------------------------------------------------------*/

/** Queue blocking work and wake one worker.
 *
 * @return Nonzero when queued, or zero when the bounded queue is full.
 */
int net_reactor_job_push(NetReactor* reactor, int fd, JobType type);

/** Take the next job, waiting for one.
 *
 * @return Nonzero when a job was taken, or zero once the reactor is stopping.
 */
int net_reactor_job_pop(NetReactor* reactor, Job* out);

/** Wake every worker so they can notice the reactor is stopping. */
void net_reactor_wake_workers(NetReactor* reactor);

/** The worker thread body. */
void* net_reactor_worker_main(void* reactor);

/** The event loop thread body. */
void* net_reactor_loop_main(void* loop);

#endif // NET_REACTOR_INTERNAL_H
