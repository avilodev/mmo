#ifndef RATE_LIMITER_H
#define RATE_LIMITER_H

#include <stddef.h>

// Longest textual address we store, including IPv6 and the NUL terminator.
#define RL_IP_MAXLEN 46

// Initialize the rate limiter. Call once at startup, before any thread runs.
void rate_limiter_init(void);

// Returns 1 if this address is currently blocked and must not be served,
// 0 if it is allowed. Never creates an entry.
int rate_limiter_check(const char* ip);

// Record a failed auth attempt from this address.
//
// Returns 1 if the caller should drop the connection immediately — either the
// address just crossed the failure threshold, or it could not be tracked at
// all. Refusing to serve an address we cannot police is the safe direction
// under a distributed attack, so an untrackable failure fails closed.
int rate_limiter_record_failure(const char* ip);

// Record one accepted connection from this address.
//
// Returns 1 if the connection should be refused. The login server closes each
// connection after a single packet, so packet floods are already impossible
// there — connection churn is the open axis, and this is what bounds it.
int rate_limiter_record_connection(const char* ip);

// Clear accumulated failures after a successful auth, so a legitimate user who
// mistyped their password a few times does not stay one slip away from a block.
void rate_limiter_note_success(const char* ip);

// Write the textual peer address of `fd` (IPv4 or IPv6) into `out`.
// out[0] is '\0' if the address could not be determined; callers must treat
// that as untrusted rather than as a valid identity.
void rate_limiter_peer_ip(int fd, char* out, size_t out_size);

#endif // RATE_LIMITER_H
