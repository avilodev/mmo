#ifndef PACKET_LIMITER_H
#define PACKET_LIMITER_H

#include <stdint.h>

// ============================================================================
// packet_limiter.h — Per-connection inbound packet budget for the world server
//
// The login server rate-limits authentication attempts, but once a client is
// authenticated into a world it could previously send any opcode as fast as the
// socket allowed. This module gives every connection a token bucket per opcode
// class, so a flood costs the sender their own budget instead of the server's
// CPU, database connections, and broadcast bandwidth.
//
// Budgets are deliberately well above what a well-behaved client needs:
// movement is capped far above the 60Hz send rate, so normal play never sees a
// drop. Sustained abuse past the budget trips a violation counter, and enough
// violations tell the caller to close the connection.
//
// Threading: each connection is serviced start-to-finish by a single worker
// thread, and every entry point here is called from that thread, so per-fd
// state needs no locking. Nothing else may call into this module for an fd it
// does not own.
// ============================================================================

typedef enum {
    PACKET_LIMIT_ALLOW = 0,   // within budget, process normally
    PACKET_LIMIT_DROP  = 1,   // over budget, silently discard this packet
    PACKET_LIMIT_KICK  = 2    // sustained abuse, caller should close the socket
} PacketLimitVerdict;

void packet_limiter_init(void);

// Prepare (or reset) the budget for a connection. Call on connect and on
// disconnect so a recycled file descriptor never inherits an old budget.
void packet_limiter_reset(int fd);

// Account for one inbound packet and decide what to do with it.
PacketLimitVerdict packet_limiter_check(int fd, uint8_t packet_type);

#endif // PACKET_LIMITER_H
