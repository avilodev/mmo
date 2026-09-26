/**
 * @file
 * The blocking pool: a bounded queue and the fixed set of threads that drain it.
 *
 * This is where "a surge becomes a queue rather than a thread per player" is
 * actually true. The pool size is how much blocking work may be in flight at
 * once, and the queue is sized so it can never overflow -- see the note on
 * job_cap in net_reactor.c, and what an overflow would otherwise discard.
 */

#include "net_reactor_internal.h"
#include "log.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/* --- The job queue ------------------------------------------------------- */

/**
 * Queue blocking work and wake one worker.
 *
 * @return Nonzero when queued, or zero when the bounded queue is full.
 */
int net_reactor_job_push(NetReactor* reactor, int fd, JobType type) {
    pthread_mutex_lock(&reactor->job_lock);
    if (reactor->job_count >= reactor->job_cap) {
        pthread_mutex_unlock(&reactor->job_lock);
        LOG_ERROR("[%s] blocking queue full (%d), dropping %s for fd %d",
                  reactor->config.name, reactor->job_cap,
                  type == JOB_WORK ? "work" : "retire", fd);
        return 0;
    }
    reactor->jobs[reactor->job_tail] = (Job){ .fd = fd, .type = type };
    reactor->job_tail = (reactor->job_tail + 1) % reactor->job_cap;
    reactor->job_count++;
    pthread_cond_signal(&reactor->job_ready);
    pthread_mutex_unlock(&reactor->job_lock);
    return 1;
}

/**
 * Take the next job, waiting for one.
 *
 * @return Nonzero when a job was taken, or zero once the reactor is stopping.
 */
int net_reactor_job_pop(NetReactor* reactor, Job* out) {
    pthread_mutex_lock(&reactor->job_lock);
    while (reactor->job_count == 0 && atomic_load(&reactor->running)) {
        pthread_cond_wait(&reactor->job_ready, &reactor->job_lock);
    }
    if (reactor->job_count == 0) {
        pthread_mutex_unlock(&reactor->job_lock);
        return 0;
    }
    *out = reactor->jobs[reactor->job_head];
    reactor->job_head = (reactor->job_head + 1) % reactor->job_cap;
    reactor->job_count--;
    pthread_mutex_unlock(&reactor->job_lock);
    return 1;
}

/* --- Workers ------------------------------------------------------------- */

/** Run the owner's blocking teardown, then release the connection to its loop. */
static void run_retire(NetReactorConn* conn) {
    if (conn->reactor->config.on_retire) conn->reactor->config.on_retire(conn);

    /* Unregistering, closing and freeing all belong to the owning loop. Doing
     * any of it here would race the idle sweep and could hand a live descriptor
     * number back to accept() while its slot was still occupied. */
    atomic_store(&conn->state, CONN_DEAD);
}

/** Run the owner's blocking work and hand the connection back, or retire it. */
static void run_work(NetReactorConn* conn) {
    NetReactorVerdict verdict = NET_REACTOR_CLOSE;
    if (conn->reactor->config.on_work) verdict = conn->reactor->config.on_work(conn);

    if (verdict == NET_REACTOR_KEEP) {
        atomic_store(&conn->last_recv, net_reactor_now());
        /* Ownership returns to the loop only once the state says so, so the
         * loop can never observe a half-finished handoff. */
        atomic_store(&conn->state, CONN_LOOP);
        if (net_reactor_arm(conn)) return;
        atomic_store(&conn->state, CONN_RETIRING);
    }

    run_retire(conn);
}

void* net_reactor_worker_main(void* arg) {
    NetReactor* reactor = (NetReactor*)arg;
    Job job;

    while (net_reactor_job_pop(reactor, &job)) {
        /* Cleared before every job, not after.
         *
         * Workers are pooled: the thread that admits one player then handles
         * the next, and a correlation id left behind attributes the second
         * player's lines to the first. That is worse than no id at all --
         * a wrong answer rather than a missing one. Clearing on entry also
         * covers a handler that returns early without clearing. */
        log_clear_trace();

        NetReactorConn* conn = net_reactor_conn_at(reactor, job.fd);
        if (!conn) continue;
        if (job.type == JOB_WORK) run_work(conn);
        else                      run_retire(conn);
    }

    log_clear_trace();
    return NULL;
}

void net_reactor_wake_workers(NetReactor* reactor) {
    pthread_mutex_lock(&reactor->job_lock);
    pthread_cond_broadcast(&reactor->job_ready);
    pthread_mutex_unlock(&reactor->job_lock);
}
