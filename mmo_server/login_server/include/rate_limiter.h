#ifndef RATE_LIMITER_H
#define RATE_LIMITER_H

/** @file Limit login attempts and accepted connections by peer address. */

#include <stddef.h>

/** Hold a terminated textual IPv6 address. */
#define RL_IP_MAXLEN 46

/** Hold a bucket key: a textual address plus a "/nnn" prefix suffix. */
#define RL_KEY_MAXLEN (RL_IP_MAXLEN + 5)

/** Default IPv6 prefix length the limiter counts against.
 *
 * An IPv6 host is routinely handed a whole /64, and often a /56 or /48, so
 * limiting per /128 -- one exact address -- limits nothing at all: an attacker
 * with a routed /64 has 18 quintillion distinct "addresses" and never touches a
 * budget. Overridable with $MMO_LOGIN_IPV6_PREFIX for a deployment whose
 * upstream allocates differently.
 */
#define RL_IPV6_PREFIX_DEFAULT 64

// initialize before starting worker threads
void rate_limiter_init(void);

// query blocking state without creating an entry
int rate_limiter_check(const char* ip);

// fail closed when an address cannot be tracked
int rate_limiter_record_failure(const char* ip);

// return whether an accepted connection should be refused
int rate_limiter_record_connection(const char* ip);

/** Charge one registration attempt against an address's hourly budget.
 *
 * Charged on the attempt, not on the outcome: a unique username always
 * succeeded, so a limiter that only counted failures never saw account
 * creation at all. Overridable with $MMO_LOGIN_MAX_REGISTRATIONS_PER_HOUR.
 *
 * @return Nonzero when the registration must be refused.
 */
int rate_limiter_record_registration(const char* ip);

// clear accumulated failures after successful authentication
void rate_limiter_note_success(const char* ip);

// write an empty string when the peer address is unavailable
void rate_limiter_peer_ip(int fd, char* out, size_t out_size);

/** Reduce a textual peer address to the key the limiter counts against.
 *
 * IPv4 addresses are their own key. IPv6 addresses are masked to the
 * configured prefix, and an IPv4-mapped IPv6 address (::ffff:a.b.c.d) is
 * reduced to the IPv4 address it carries, so the same host is one bucket
 * whichever way the socket reports it.
 *
 * Exposed for tests; callers of the limiter pass raw addresses and this is
 * applied for them.
 *
 * @return 1 when a key was written, or 0 when the address is unusable.
 */
int rate_limiter_bucket_key(const char* ip, char* out, size_t out_size);

/** Entries the limiter table currently holds. For tests and metrics. */
size_t rate_limiter_tracked(void);

/** Slots the limiter table has grown to. For tests and metrics. */
size_t rate_limiter_capacity(void);

#endif // RATE_LIMITER_H
