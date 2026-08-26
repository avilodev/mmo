/**
 * @file
 * Parse CIDR rules and match peer addresses against them.
 */
#include "ip_allowlist.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** Bytes of address for a family, or 0 when the family is unsupported. */
static int family_bytes(int family) {
    if (family == AF_INET)  return 4;
    if (family == AF_INET6) return 16;
    return 0;
}

/** Clear the rule set. */
void ip_allowlist_reset(IpAllowlist* list) {
    if (!list) return;
    memset(list, 0, sizeof(*list));
}

/** Report an empty rule set. */
int ip_allowlist_is_empty(const IpAllowlist* list) {
    return !list || list->count == 0;
}

/**
 * Parse "address" or "address/prefix" into one rule.
 *
 * Parsing is strict on purpose: a rule an operator mistyped must be refused
 * loudly rather than stored as something narrower or wider than intended.
 */
int ip_allowlist_add(IpAllowlist* list, const char* rule) {
    if (!list || !rule || list->count >= IP_ALLOWLIST_MAX_RULES) return 0;

    // Split off an optional /prefix without mutating the caller's string.
    char address_text[64];
    const char* slash = strchr(rule, '/');
    size_t address_length = slash ? (size_t)(slash - rule) : strlen(rule);
    if (address_length == 0 || address_length >= sizeof(address_text)) return 0;
    memcpy(address_text, rule, address_length);
    address_text[address_length] = '\0';

    int family = AF_INET;
    uint8_t parsed[16] = {0};
    if (inet_pton(AF_INET, address_text, parsed) != 1) {
        family = AF_INET6;
        if (inet_pton(AF_INET6, address_text, parsed) != 1) return 0;
    }

    const int width_bits = family_bytes(family) * 8;
    int prefix_bits = width_bits;

    if (slash) {
        const char* digits = slash + 1;
        if (*digits == '\0') return 0;

        char* end = NULL;
        long parsed_prefix = strtol(digits, &end, 10);
        if (!end || *end != '\0') return 0;
        if (parsed_prefix < 0 || parsed_prefix > width_bits) return 0;
        prefix_bits = (int)parsed_prefix;
    }

    IpAllowRule* stored = &list->rules[list->count];
    stored->family      = family;
    stored->prefix_bits = (uint8_t)prefix_bits;
    memcpy(stored->addr, parsed, sizeof(stored->addr));
    list->count++;
    return 1;
}

/** Compare the leading prefix_bits of two address byte arrays. */
static int prefix_matches(const uint8_t* a, const uint8_t* b, int prefix_bits) {
    int whole_bytes = prefix_bits / 8;
    int spare_bits  = prefix_bits % 8;

    if (whole_bytes > 0 && memcmp(a, b, (size_t)whole_bytes) != 0) return 0;
    if (spare_bits == 0) return 1;

    uint8_t mask = (uint8_t)(0xFFu << (8 - spare_bits));
    return (a[whole_bytes] & mask) == (b[whole_bytes] & mask);
}

/** Report whether an IPv6 address carries an IPv4-mapped address. */
static int is_v4_mapped(const uint8_t* v6) {
    static const uint8_t mapped_prefix[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF};
    return memcmp(v6, mapped_prefix, sizeof(mapped_prefix)) == 0;
}

/** Match one already-extracted address against the set. */
static int allowlist_match(const IpAllowlist* list, int family, const uint8_t* addr) {
    for (int i = 0; i < list->count; i++) {
        const IpAllowRule* rule = &list->rules[i];

        if (rule->family == family) {
            if (prefix_matches(addr, rule->addr, rule->prefix_bits)) return 1;
            continue;
        }

        /* A dual-stack listener reports an IPv4 peer as ::ffff:a.b.c.d. Without
         * this the same peer matches an IPv4 rule on one listener and nothing on
         * the other, which is the kind of difference that only shows up in
         * production. */
        if (family == AF_INET6 && rule->family == AF_INET && is_v4_mapped(addr)) {
            if (prefix_matches(addr + 12, rule->addr, rule->prefix_bits)) return 1;
        }
    }
    return 0;
}

/** Match a socket address against the set. */
int ip_allowlist_contains(const IpAllowlist* list, const struct sockaddr* addr) {
    if (ip_allowlist_is_empty(list) || !addr) return 0;

    if (addr->sa_family == AF_INET) {
        const struct sockaddr_in* v4 = (const struct sockaddr_in*)addr;
        return allowlist_match(list, AF_INET, (const uint8_t*)&v4->sin_addr);
    }
    if (addr->sa_family == AF_INET6) {
        const struct sockaddr_in6* v6 = (const struct sockaddr_in6*)addr;
        return allowlist_match(list, AF_INET6, (const uint8_t*)&v6->sin6_addr);
    }
    return 0;
}

/** Match a textual address against the set. */
int ip_allowlist_contains_text(const IpAllowlist* list, const char* address_text) {
    if (ip_allowlist_is_empty(list) || !address_text || !address_text[0]) return 0;

    uint8_t parsed[16] = {0};
    if (inet_pton(AF_INET, address_text, parsed) == 1)
        return allowlist_match(list, AF_INET, parsed);
    if (inet_pton(AF_INET6, address_text, parsed) == 1)
        return allowlist_match(list, AF_INET6, parsed);
    return 0;
}

/** Render the rule set for a log line. */
void ip_allowlist_describe(const IpAllowlist* list, char* out, size_t out_size) {
    if (!out || out_size == 0) return;
    out[0] = '\0';

    if (ip_allowlist_is_empty(list)) {
        snprintf(out, out_size, "(empty — nothing is allowed)");
        return;
    }

    size_t used = 0;
    for (int i = 0; i < list->count && used + 1 < out_size; i++) {
        char text[INET6_ADDRSTRLEN] = {0};
        inet_ntop(list->rules[i].family, list->rules[i].addr, text, sizeof(text));

        int written = snprintf(out + used, out_size - used, "%s%s/%u",
                               i == 0 ? "" : ", ", text, list->rules[i].prefix_bits);
        if (written < 0) break;
        used += (size_t)written;
        if (used >= out_size) break;
    }
}
