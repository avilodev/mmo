#ifndef NET_REACTOR_H
#define NET_REACTOR_H

/** @file Run connections on pinned epoll loops with a bounded blocking pool.
 *
 * This is the accept path with the game taken out of it. It owns descriptors,
 * epoll loops, reassembly buffers, the idle sweep, and a bounded worker pool for
 * work that blocks; it knows nothing about players, sessions, quests, packets,
 * or TLS. What to do with the bytes is the owner's, supplied as callbacks.
 *
 * The shape it exists to replace is thread-per-connection. One thread per
 * connecting player is fine until the day everybody connects at once, which for
 * a login server is launch day and every patch day after it, and there is
 * nothing in that design to bound the total. Here the thread count is fixed at
 * startup: a loop per core, and a worker pool whose depth is how much blocking
 * work may be in flight at once. A surge becomes a queue, and a queue that
 * overflows sheds load deliberately instead of running the machine out of
 * stacks.
 *
 * ## Threading contract
 *
 * A connection is pinned to one loop by `fd % loop_count` for its whole life and
 * is never migrated. At any moment it is owned by exactly one thread:
 *
 * - its loop, which is the only thread that may read it, close its descriptor,
 *   or free it, or
 * - a worker, between the loop detaching it and the worker handing it back.
 *
 * `on_accept`, `on_data`, `on_writable`, `on_idle` and `on_reap` run on the
 * loop and must not block. `on_work` and `on_retire` run on a worker and may
 * block for as long as they need to.
 *
 * ## Transports
 *
 * Reading is done through an optional transport, so a TLS connection is the same
 * object as a plain one with a different `recv` and a handshake that has to be
 * driven across several events. With no transport the reactor uses recv(2).
 */

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/** One connection. Opaque: the owner's own state lives behind conn_user(). */
typedef struct NetReactorConn NetReactorConn;

/** One running set of loops and workers. */
typedef struct NetReactor NetReactor;

/** Say what should happen to a connection after a callback. */
typedef enum {
    NET_REACTOR_KEEP = 0,    /**< Leave it on its loop. */
    NET_REACTOR_CLOSE,       /**< Retire it: on_retire, then on_reap. */
    NET_REACTOR_TO_WORKER    /**< Detach it and run on_work on a worker. */
} NetReactorVerdict;

/** Read bytes off a connection however that connection actually works.
 *
 * Every function here runs on the connection's loop thread.
 */
typedef struct {
    /** Drive a handshake that is not finished yet.
     *
     * Called before any on_data, and called again on every readable or writable
     * event until it stops asking for more. A handshake that needs to write
     * gets its chance because both directions are always armed.
     *
     * @return 1 when the connection is ready to carry data, 0 to be called
     *         again on the next event, or -1 to close.
     */
    int (*handshake)(NetReactorConn* conn);

    /** Read into a buffer.
     *
     * Must follow recv(2): a positive count, 0 for a closed peer, or -1 with
     * errno set, using EAGAIN/EWOULDBLOCK to mean "drained for now".
     */
    ssize_t (*recv)(NetReactorConn* conn, void* buf, size_t len);

    /** Release whatever the transport attached to this connection.
     *
     * Called on the loop thread just before the descriptor is closed, on every
     * path a connection can leave by -- including one refused by on_accept, so
     * it must tolerate a connection the transport never got to set up.
     */
    void (*close)(NetReactorConn* conn);
} NetReactorTransport;

/** Configure one reactor. Copied at startup; the caller keeps nothing alive. */
typedef struct {
    const char* name;             /**< Used in log lines. */

    /** Loops to run, or 0 for one per core. Capped by the build. */
    int    loop_count;
    /** Threads available for blocking work, or 0 for the default. */
    int    worker_count;
    /** Blocking jobs that may be waiting at once, or 0 for the default.
     *
     * Raised to the connection-table size whatever is asked for, because a
     * connection has at most one job outstanding and a queue that cannot
     * overflow is one that cannot drop a teardown. Load is shed by the size of
     * the worker pool -- a surge waits -- not by discarding work. */
    int    job_queue_cap;
    /** Bytes of reassembly buffer per connection, or 0 for the default.
     *
     * A connection whose buffer fills without the owner consuming anything is
     * closed: it is not speaking a protocol the owner understands. */
    size_t buffer_size;
    /** Bytes of owner state per connection, reachable with conn_user(). */
    size_t user_size;
    /** Seconds between idle sweeps, or 0 for the default. */
    int    sweep_interval_secs;

    /** How to read. NULL for plain sockets. */
    const NetReactorTransport* transport;

    /** Loop thread. A connection has just been armed.
     *
     * @return 0 to keep it, or -1 to refuse it before it ever reads a byte.
     */
    int (*on_accept)(NetReactorConn* conn);

    /** Loop thread. Everything buffered so far, as one contiguous run.
     *
     * @param consumed  Set to how many leading bytes were dealt with. What is
     *                  left is carried over and offered again with the next
     *                  arrival, so a partial packet costs nothing to leave.
     */
    NetReactorVerdict (*on_data)(NetReactorConn* conn, uint8_t* data, size_t len,
                                 size_t* consumed);

    /** Loop thread. The socket became writable. Optional.
     *
     * @return 0 to keep the connection, or -1 to close it.
     */
    int (*on_writable)(NetReactorConn* conn);

    /** Worker thread. Do the blocking work that NET_REACTOR_TO_WORKER asked for.
     *
     * @return NET_REACTOR_KEEP to hand the connection back to its loop, or
     *         NET_REACTOR_CLOSE to retire it.
     */
    NetReactorVerdict (*on_work)(NetReactorConn* conn);

    /** Worker thread. Blocking teardown: saves, database writes, deregistration.
     *
     * Runs for every connection that closes, authenticated or not, exactly once
     * -- including connections still open when net_reactor_stop() is called,
     * where it runs on the stopping thread rather than on a worker, after every
     * loop and worker has been joined.
     *
     * The descriptor is still open here and may still be written to. Optional.
     */
    void (*on_retire)(NetReactorConn* conn);

    /** Loop thread. Last call before the descriptor is closed and the
     *  connection freed. For per-descriptor state the owner keeps elsewhere.
     *
     *  The connection is still counted while this runs: the count is what tells
     *  another thread the teardown is finished, so it does not fall until it is.
     *  Optional. */
    void (*on_reap)(NetReactorConn* conn);

    /** Loop thread, once per sweep, for connections the loop owns. Optional.
     *
     * @param idle_seconds  Since the last byte arrived.
     * @return Nonzero to close the connection.
     */
    int (*on_idle)(NetReactorConn* conn, long idle_seconds);
} NetReactorConfig;

/** Start the loops and workers.
 *
 * @return A running reactor, or NULL when allocation or thread setup failed.
 */
NetReactor* net_reactor_start(const NetReactorConfig* config);

/** Stop every loop and worker, join them, and release every connection.
 *
 * Safe on NULL. After this returns nothing else is running.
 */
void net_reactor_stop(NetReactor* reactor);

/** Hand an accepted descriptor to its pinned loop.
 *
 * Takes ownership: the descriptor is closed on any failure, and the caller must
 * not touch it again either way.
 */
void net_reactor_submit(NetReactor* reactor, int fd);

/** Report live connections. */
int net_reactor_connection_count(const NetReactor* reactor);

/** Report how many loops are running.
 *
 * Connections shard across loops by `fd % count`, so anything that wants to fan
 * work out along the same seams shards on this number and the same modulus.
 */
int net_reactor_loop_count(const NetReactor* reactor);

/** Blocking jobs waiting for a worker. For the metrics endpoint.
 *
 * A depth that stays near capacity means the workers are not keeping up with
 * what the loops are handing them, which is the shape a database stall or a
 * slow save takes as seen from the network side. It briefly takes the job
 * lock, so scrape it, do not poll it.
 *
 * @param out_capacity  Receives the queue's capacity. May be NULL.
 * @return              Jobs currently queued.
 */
int net_reactor_job_depth(NetReactor* reactor, int* out_capacity);

/* --- One connection ------------------------------------------------------ */

int net_reactor_conn_fd(const NetReactorConn* conn);

/** Reach the owner's per-connection state.
 *
 * `user_size` bytes, zeroed before on_accept, valid until on_reap returns.
 */
void* net_reactor_conn_user(NetReactorConn* conn);

/** Reach the reassembly buffer, for a worker that needs the bytes on_data saw.
 *
 * @param out_len  Receives how many bytes are buffered.
 */
uint8_t* net_reactor_conn_buffer(NetReactorConn* conn, size_t* out_len);

/** Drop leading bytes from the reassembly buffer.
 *
 * What on_data does through its `consumed` parameter, for a worker that has
 * dealt with a prefix of the buffer itself.
 */
void net_reactor_conn_consume(NetReactorConn* conn, size_t bytes);

#endif // NET_REACTOR_H
