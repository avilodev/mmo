#ifndef IP_ALLOWLIST_H
#define IP_ALLOWLIST_H

/** @file Match peer addresses against a configured set of IPv4/IPv6 CIDR rules.
 *
 * Server-to-server ports must not accept a handshake from anywhere on the
 * internet. The rule set is small, fixed-size, and copied by value so it can be
 * built at startup and read from accept threads without a lock.
 */

#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>

/** Bound the rule set; deployments list a handful of realm hosts, not a table. */
#define IP_ALLOWLIST_MAX_RULES 32

/** One parsed CIDR rule, stored as a network-order address plus a prefix length. */
typedef struct {
    int      family;        /**< AF_INET or AF_INET6. */
    uint8_t  prefix_bits;   /**< 0..32 for IPv4, 0..128 for IPv6. */
    uint8_t  addr[16];      /**< Network-order address bytes; 4 used for IPv4. */
} IpAllowRule;

/** A fixed-capacity set of CIDR rules. Zeroing it produces an empty set. */
typedef struct {
    IpAllowRule rules[IP_ALLOWLIST_MAX_RULES];
    int         count;
} IpAllowlist;

/** Empty the rule set. */
void ip_allowlist_reset(IpAllowlist* list);

/** Report whether the set holds no rules. An empty set matches nothing. */
int ip_allowlist_is_empty(const IpAllowlist* list);

/**
 * Parse and append one rule.
 *
 * Accepts a bare address ("10.0.0.4", "::1") or CIDR notation ("10.0.0.0/24",
 * "fd00::/8"). A bare address is treated as a host route (/32 or /128).
 *
 * @return 1 when the rule was parsed and stored, or 0 on a malformed rule,
 *         an out-of-range prefix, or a full set.
 */
int ip_allowlist_add(IpAllowlist* list, const char* rule);

/**
 * Test a peer address against the set.
 *
 * IPv4-mapped IPv6 peers (::ffff:a.b.c.d) are matched against IPv4 rules, so a
 * dual-stack listener does not silently bypass an IPv4 allowlist.
 *
 * @return 1 when some rule covers the address, otherwise 0. An empty set,
 *         a NULL argument, or an unsupported family all return 0.
 */
int ip_allowlist_contains(const IpAllowlist* list, const struct sockaddr* addr);

/** Test a peer address given as text, for callers that already resolved it. */
int ip_allowlist_contains_text(const IpAllowlist* list, const char* address_text);

/**
 * Describe the set for a startup log line.
 *
 * @param out       Buffer receiving a comma-separated rule list.
 * @param out_size  Size of that buffer.
 */
void ip_allowlist_describe(const IpAllowlist* list, char* out, size_t out_size);

#endif // IP_ALLOWLIST_H
