#ifndef NET_LOOP_H
#define NET_LOOP_H

#include <stdint.h>

// ============================================================================
// net_loop.h — epoll-based connection handling for the world server.
//
// Replaces one-thread-per-connection. A small number of event loops (one per
// core, capped) each watch thousands of sockets, so the number of players a
// world can hold stops being the number of threads it can afford.
//
// OWNERSHIP
//
// Exactly one entity owns a connection at any moment, and ownership is handed
// over explicitly rather than shared:
//
//   loop   -> reading, parsing, dispatching packets
//   worker -> the blocking database work at session start and session end
//
// Before a connection is handed to a worker it is removed from its epoll set,
// so the loop cannot touch it while the worker has it. This is what keeps the
// packet limiter lock-free: a connection is still serviced by one thread at a
// time, exactly as it was under thread-per-connection.
//
// AFFINITY
//
// A connection is pinned to loop (fd % loop_count) for its entire life and is
// never migrated. Two loops never contend for the same connection's state.
//
// WHY A WORKER POOL AT ALL
//
// Steady-state gameplay does no synchronous database work — saves are batched
// by the periodic save thread in player_data.c. Only session start (validate
// ticket, verify ownership, load character) and session end (write character
// and quests) block, and those are once per session. Running them on a loop
// would stall every other connection sharing it, so they are handed off.
// ============================================================================

// Start the event loops and the blocking worker pool.
// Returns 0 on success, -1 on failure.
int net_loop_start(void);

// Stop accepting work, wake every loop and worker, and join them.
void net_loop_stop(void);

// Hand an accepted socket to the event loops. Takes ownership of fd: it will
// be closed by the loop or a worker, never by the caller.
void net_loop_submit(int fd);

// Number of connections currently tracked. For logging and status packets.
int net_loop_connection_count(void);

#endif // NET_LOOP_H
