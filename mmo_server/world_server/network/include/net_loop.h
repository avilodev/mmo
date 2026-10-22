#ifndef NET_LOOP_H
#define NET_LOOP_H

#include <stdint.h>

/** @file Dispatch world sockets through epoll loops and blocking session workers.
 * Each connection remains single-thread-owned and pinned to one loop for its lifetime.
 */

/** Start the event loops and the blocking worker pool.
 *
 * @param worker_count  Concurrent logins and logouts, or 0 for the default.
 * @return Zero on success, or -1 on failure.
 */
int net_loop_start(int worker_count);

void net_loop_stop(void);

// transfer descriptor ownership to its assigned event loop
void net_loop_submit(int fd);

int net_loop_connection_count(void);

/** Report how many epoll loops are running.
 *
 * Connections are pinned to a loop by `fd % count` for their whole lifetime, so
 * anything that wants to fan work out along the same seams — the broadcast pool,
 * for one — shards on this number and the same modulus. Zero before startup.
 */
int net_loop_count(void);

/** Blocking jobs waiting for a worker. For the metrics endpoint.
 *
 * @param out_capacity  Receives the queue's capacity. May be NULL.
 * @return              Jobs currently queued.
 */
int net_loop_job_depth(int* out_capacity);

#endif // NET_LOOP_H
