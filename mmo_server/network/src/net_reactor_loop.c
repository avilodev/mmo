/**
 * @file
 * The event loops: readiness, handshakes, reading, and the idle sweep.
 *
 * Everything in this file runs on a loop thread and must not block. What cannot
 * be done without blocking is handed to a worker instead, which is the one
 * decision this file exists to make.
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

/* --- Reading ------------------------------------------------------------- */

/** Read through the transport, or straight off the socket when there is none. */
static ssize_t conn_recv(NetReactorConn* conn, void* buf, size_t len) {
    const NetReactorTransport* transport = conn->reactor->config.transport;
    if (transport && transport->recv) return transport->recv(conn, buf, len);
    return recv(conn->fd, buf, len, 0);
}

/**
 * Offer everything buffered to the owner and drop what it consumed.
 *
 * @return 0 to keep loop ownership, -1 to close, or 1 after a worker handoff.
 */
static int deliver(NetReactorConn* conn) {
    NetReactor* reactor = conn->reactor;
    if (!reactor->config.on_data || conn->buf_len == 0) return 0;

    size_t consumed = 0;
    NetReactorVerdict verdict =
        reactor->config.on_data(conn, conn->buf, conn->buf_len, &consumed);

    if (consumed > conn->buf_len) consumed = conn->buf_len;
    if (consumed > 0) {
        conn->buf_len -= consumed;
        if (conn->buf_len > 0) memmove(conn->buf, conn->buf + consumed, conn->buf_len);
    }

    if (verdict == NET_REACTOR_CLOSE) return -1;
    if (verdict == NET_REACTOR_TO_WORKER) {
        net_reactor_hand_off(conn, CONN_WORKING, JOB_WORK);
        return 1;
    }
    return 0;
}

/**
 * Drain an edge-triggered socket, delivering as it goes.
 *
 * @return 0 to keep loop ownership, -1 to close, or 1 after a worker handoff.
 */
static int loop_read(NetReactorConn* conn) {
    /* Whatever is already buffered is offered first.
     *
     * A worker can hand a connection back with bytes still in the buffer -- the
     * client coalesced them behind the packet the worker was given -- and
     * readiness is edge-triggered, so the socket will never mention data that
     * was taken off it before the handoff. Without this, those bytes wait for
     * the peer to send something else, which for a client waiting on a reply
     * is never. */
    int pending = deliver(conn);
    if (pending != 0) return pending;

    while (1) {
        size_t space = conn->reactor->buffer_size - conn->buf_len;
        if (space == 0) {
            /* Full without the owner taking anything: the peer is not speaking
             * a protocol this owner frames. Growing the buffer here would make
             * that a memory cost per hostile connection. */
            LOG_WARN("[%s] fd=%d filled its buffer without a complete message — closing",
                     conn->reactor->config.name, conn->fd);
            return -1;
        }

        ssize_t bytes = conn_recv(conn, conn->buf + conn->buf_len, space);
        if (bytes > 0) {
            conn->buf_len += (size_t)bytes;
            atomic_store(&conn->last_recv, net_reactor_now());

            int r = deliver(conn);
            if (r != 0) return r;
            continue;
        }

        if (bytes == 0) return -1;                              /* peer closed */
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;  /* drained */
        return -1;
    }
}

/**
 * Drive a transport handshake that has not finished.
 *
 * @return 0 when the connection is ready to read, 1 to wait for another event,
 *         or -1 to close.
 */
static int loop_handshake(NetReactorConn* conn) {
    const NetReactorTransport* transport = conn->reactor->config.transport;
    if (conn->ready || !transport || !transport->handshake) return 0;

    int result = transport->handshake(conn);
    if (result < 0) return -1;
    if (result == 0) return 1;

    conn->ready = 1;
    atomic_store(&conn->last_recv, net_reactor_now());
    return 0;
}

/* --- Loops --------------------------------------------------------------- */

/** Reap finished connections and close the ones the owner calls idle. */
static void sweep(NetReactor* reactor, int loop_id) {
    long now = net_reactor_now();

    for (int fd = loop_id; fd < reactor->conn_count; fd += reactor->loop_count) {
        NetReactorConn* conn =
            atomic_load_explicit(&reactor->conns[fd], memory_order_acquire);
        if (!conn) continue;

        int state = atomic_load(&conn->state);
        if (state == CONN_DEAD) { net_reactor_reap(conn); continue; }

        /* Anything else that is not loop-owned is mid-handoff to a worker. */
        if (state != CONN_LOOP) continue;

        if (reactor->config.on_idle) {
            long idle = now - atomic_load(&conn->last_recv);
            if (reactor->config.on_idle(conn, idle)) {
                net_reactor_close_from_loop(conn);
                continue;
            }
        }
    }
}

void* net_reactor_loop_main(void* arg) {
    ReactorLoop* loop = (ReactorLoop*)arg;
    NetReactor*  reactor = loop->reactor;
    struct epoll_event events[REACTOR_EVENTS];
    long last_sweep = net_reactor_now();

    LOG_INFO("[%s] event loop %d started", reactor->config.name, loop->id);

    while (atomic_load(&reactor->running)) {
        int n = epoll_wait(loop->epfd, events, REACTOR_EVENTS, REACTOR_TICK_MS);
        if (n < 0) {
            if (errno == EINTR) continue;
            LOG_ERROR("[%s] epoll_wait failed on loop %d: %s",
                      reactor->config.name, loop->id, strerror(errno));
            break;
        }

        for (int i = 0; i < n; i++) {
            NetReactorConn* conn = net_reactor_conn_at(reactor, events[i].data.fd);
            if (!conn) continue;
            if (atomic_load(&conn->state) != CONN_LOOP) continue;

            uint32_t ev = events[i].events;

            /* A socket error has no readable remainder worth having: recv()
             * would return the error rather than data. Close it now. */
            if (ev & EPOLLERR) { net_reactor_close_from_loop(conn); continue; }

            /* A hangup is not an error, and it is not a reason to throw away
             * bytes that already arrived.
             *
             * EPOLLHUP used to close the connection on the spot, alongside
             * EPOLLERR. But a peer that sends its last packet and then shuts
             * down -- a client sending LOGOUT and closing, which is the polite
             * path -- can have both the data and the hangup reported in the
             * same event. Closing immediately discarded the LOGOUT, so the
             * orderly disconnect took the same route as a dropped one.
             *
             * So the hangup is remembered and the read path still runs; the
             * close happens after the buffer is drained. loop_read() usually
             * reaches recv() == 0 and asks for the close itself, but the
             * explicit `hangup` close below covers the cases where it does not
             * -- a socket that reports EAGAIN after the drain, or a transport
             * handshake that will now never finish. */
            int hangup = (ev & EPOLLHUP) != 0;

            /* Flush before reading: a writable transition may be the only
             * notice that a queued write can go out. */
            if ((ev & EPOLLOUT) && reactor->config.on_writable) {
                if (reactor->config.on_writable(conn) != 0) { net_reactor_close_from_loop(conn); continue; }
            }

            if (!(ev & (EPOLLIN | EPOLLOUT | EPOLLRDHUP)) && !hangup) continue;

            int handshake = loop_handshake(conn);
            if (handshake < 0) { net_reactor_close_from_loop(conn); continue; }
            if (handshake > 0) {
                /* Not ready yet -- and if the peer has hung up it never will
                 * be, so do not leave it for the idle sweep. */
                if (hangup) net_reactor_close_from_loop(conn);
                continue;
            }

            int r = loop_read(conn);
            if (r == -1) { net_reactor_close_from_loop(conn); continue; }
            if (r == 1)  continue;         /* handed to a worker */
            if (hangup)  { net_reactor_close_from_loop(conn); continue; }
        }

        long now = net_reactor_now();
        if (now - last_sweep >= reactor->sweep_secs) {
            sweep(reactor, loop->id);
            last_sweep = now;
        }
    }

    LOG_INFO("[%s] event loop %d exiting", reactor->config.name, loop->id);
    return NULL;
}
