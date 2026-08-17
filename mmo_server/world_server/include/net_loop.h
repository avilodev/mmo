#ifndef NET_LOOP_H
#define NET_LOOP_H

#include <stdint.h>

/** @file Dispatch world sockets through epoll loops and blocking session workers.
 * Each connection remains single-thread-owned and pinned to one loop for its lifetime.
 */

// return zero on startup success or -1 on failure
int net_loop_start(void);

void net_loop_stop(void);

// transfer descriptor ownership to its assigned event loop
void net_loop_submit(int fd);

int net_loop_connection_count(void);

#endif // NET_LOOP_H
