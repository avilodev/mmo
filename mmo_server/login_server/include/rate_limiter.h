#ifndef RATE_LIMITER_H
#define RATE_LIMITER_H

/** @file Limit login attempts and accepted connections by peer address. */

#include <stddef.h>

/** Hold a terminated textual IPv6 address. */
#define RL_IP_MAXLEN 46

// initialize before starting worker threads
void rate_limiter_init(void);

// query blocking state without creating an entry
int rate_limiter_check(const char* ip);

// fail closed when an address cannot be tracked
int rate_limiter_record_failure(const char* ip);

// return whether an accepted connection should be refused
int rate_limiter_record_connection(const char* ip);

// clear accumulated failures after successful authentication
void rate_limiter_note_success(const char* ip);

// write an empty string when the peer address is unavailable
void rate_limiter_peer_ip(int fd, char* out, size_t out_size);

#endif // RATE_LIMITER_H
