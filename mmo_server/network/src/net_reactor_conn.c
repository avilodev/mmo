/**
 * @file
 * One connection: the descriptor table, taking a connection on, and letting go.
 *
 * The ownership rule lives here. A connection is pinned to one loop for life and
 * is owned at any instant by exactly one thread -- its loop, or a worker it has
 * been handed to. Only the loop may close a descriptor or free a connection, and
 * the ordering at the end of net_reactor_reap() is what makes a recycled
 * descriptor number safe.
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

long net_reactor_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec;
}

int net_reactor_set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/* --- Connections --------------------------------------------------------- */

NetReactorConn* net_reactor_conn_at(NetReactor* reactor, int fd) {
    if (!reactor->conns || fd < 0 || fd >= reactor->conn_count) return NULL;
    return atomic_load_explicit(&reactor->conns[fd], memory_order_acquire);
}

/**
 * Register a worker-owned connection with its pinned loop.
 *
 * Both directions are always armed, edge-triggered: a handshake that needs to
 * write gets its event without the loop having to model which way it is stuck.
 *
 * @return Nonzero when registered.
 */
int net_reactor_arm(NetReactorConn* conn) {
    struct epoll_event ev = {0};
    ev.events  = EPOLLIN | EPOLLOUT | EPOLLET | EPOLLRDHUP;
    ev.data.fd = conn->fd;
    if (epoll_ctl(conn->reactor->loops[conn->loop].epfd,
                  EPOLL_CTL_ADD, conn->fd, &ev) != 0) {
        LOG_ERROR("[%s] epoll_ctl ADD failed for fd %d: %s",
                  conn->reactor->config.name, conn->fd, strerror(errno));
        return 0;
    }
    return 1;
}

/** Detach a loop-owned connection and hand it to a worker. */
void net_reactor_hand_off(NetReactorConn* conn, ConnState next, JobType job) {
    NetReactor* reactor = conn->reactor;
    epoll_ctl(reactor->loops[conn->loop].epfd, EPOLL_CTL_DEL, conn->fd, NULL);
    atomic_store(&conn->state, next);

    if (!net_reactor_job_push(reactor, conn->fd, job)) {
        /* Unreachable: the queue holds one slot per connection and a connection
         * can only ever have one job outstanding (both callers require
         * CONN_LOOP and leave it immediately). Guarded anyway, because the
         * alternative to reaping it here is leaking the descriptor -- but note
         * what is lost. Skipping a JOB_WORK refuses a request, which is
         * deliberate load shedding. Skipping a JOB_RETIRE discards the owner's
         * teardown, which for the world server is the character's save. That is
         * data loss, not shedding, so it must stay impossible rather than
         * merely unlikely. */
        LOG_ERROR("[%s] fd %d: no room to queue %s — retiring it unhandled",
                  reactor->config.name, conn->fd,
                  job == JOB_WORK ? "work" : "teardown");
        atomic_store(&conn->state, CONN_DEAD);
    }
}

/** Send a loop-owned connection down the retire path. */
void net_reactor_close_from_loop(NetReactorConn* conn) {
    net_reactor_hand_off(conn, CONN_RETIRING, JOB_RETIRE);
}

/** Close and free a finished connection, from its owning loop. */
void net_reactor_reap(NetReactorConn* conn) {
    NetReactor* reactor = conn->reactor;
    int fd = conn->fd;

    if (reactor->config.on_reap) reactor->config.on_reap(conn);
    if (reactor->config.transport && reactor->config.transport->close)
        reactor->config.transport->close(conn);

    /* The descriptor is closed before its slot is released, and both come after
     * on_reap. Everything on_reap touches is keyed by descriptor number, and the
     * kernel will not hand that number back from accept() until this close
     * returns, so releasing the slot any earlier would let a new connection
     * claim the number while the old one's per-descriptor state was still being
     * torn down. */
    close(fd);
    atomic_store_explicit(&reactor->conns[fd], NULL, memory_order_release);

    /* Last, and deliberately so. The count is how an outside thread learns a
     * connection is gone, so it must not fall until it actually is: dropping it
     * before on_reap advertises a teardown that has not happened yet, and the
     * reader is then free to reuse whatever on_reap is still clearing. Moving
     * this one line earlier reproduces as 36 data races under
     * `make net-loop-sanitize`, because it is this release that orders the
     * world's per-descriptor cleanup against the next connection on the same
     * descriptor number. */
    atomic_fetch_sub(&reactor->live, 1);

    free(conn->buf);
    free(conn->user);
    free(conn);
}

void net_reactor_submit(NetReactor* reactor, int fd) {
    if (!reactor || fd < 0) {
        if (fd >= 0) close(fd);
        return;
    }

    if (!reactor->conns || fd >= reactor->conn_count) {
        LOG_ERROR("[%s] fd %d outside the connection table (%d) — refusing",
                  reactor->config.name, fd, reactor->conn_count);
        close(fd);
        return;
    }
    if (net_reactor_set_nonblocking(fd) != 0) {
        LOG_ERROR("[%s] could not set fd %d non-blocking: %s",
                  reactor->config.name, fd, strerror(errno));
        close(fd);
        return;
    }

    NetReactorConn* conn = calloc(1, sizeof(*conn));
    uint8_t* buf  = calloc(1, reactor->buffer_size);
    void*    user = reactor->user_size ? calloc(1, reactor->user_size) : NULL;

    if (!conn || !buf || (reactor->user_size && !user)) {
        LOG_ERROR("[%s] out of memory accepting fd %d", reactor->config.name, fd);
        free(conn); free(buf); free(user);
        close(fd);
        return;
    }

    conn->fd      = fd;
    conn->loop    = fd % reactor->loop_count;   /* pinned for life */
    conn->reactor = reactor;
    conn->buf     = buf;
    conn->user    = user;
    conn->ready   = reactor->config.transport &&
                    reactor->config.transport->handshake ? 0 : 1;
    atomic_store(&conn->state, CONN_LOOP);
    atomic_store(&conn->last_recv, net_reactor_now());

    if (reactor->config.on_accept && reactor->config.on_accept(conn) != 0) {
        /* on_accept may have allocated through the transport before deciding to
         * refuse, so the transport still gets its chance to release. on_reap
         * does not run: the owner already knows this connection never started. */
        if (reactor->config.transport && reactor->config.transport->close)
            reactor->config.transport->close(conn);
        free(conn->buf); free(conn->user); free(conn);
        close(fd);
        return;
    }

    /* Release store: a loop that sees this pointer must also see everything
     * written above it. */
    atomic_store_explicit(&reactor->conns[fd], conn, memory_order_release);
    atomic_fetch_add(&reactor->live, 1);

    if (!net_reactor_arm(conn)) {
        /* It never reached a loop, so nothing else can be looking at it. */
        atomic_store_explicit(&reactor->conns[fd], NULL, memory_order_release);
        atomic_fetch_sub(&reactor->live, 1);
        if (reactor->config.on_reap) reactor->config.on_reap(conn);
        if (reactor->config.transport && reactor->config.transport->close)
            reactor->config.transport->close(conn);
        close(fd);
        free(conn->buf); free(conn->user); free(conn);
    }
}
