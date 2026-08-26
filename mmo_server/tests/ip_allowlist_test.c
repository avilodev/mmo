/**
 * @file
 * Exercise the CIDR allowlist that guards the realm-to-world handshake.
 *
 * This is the gate that decides whether an unauthenticated peer gets a thread,
 * so the cases worth writing down are the ones where a rule might match more
 * than the operator wrote: a prefix off by one bit, a mistyped rule stored
 * anyway, or an IPv4 peer arriving over a dual-stack listener.
 */
#include "ip_allowlist.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>

static int g_failures = 0;

#define CHECK(cond, what)                                                   \
    do {                                                                    \
        if (cond) {                                                         \
            printf("  ok   %s\n", (what));                                  \
        } else {                                                            \
            printf("  FAIL %s  (%s:%d)\n", (what), __FILE__, __LINE__);     \
            g_failures++;                                                   \
        }                                                                   \
    } while (0)

/** Build an IPv4 sockaddr for a dotted-quad literal. */
static struct sockaddr_storage v4(const char* text) {
    struct sockaddr_storage ss;
    memset(&ss, 0, sizeof(ss));
    struct sockaddr_in* a = (struct sockaddr_in*)&ss;
    a->sin_family = AF_INET;
    inet_pton(AF_INET, text, &a->sin_addr);
    return ss;
}

/** Build an IPv6 sockaddr for a literal. */
static struct sockaddr_storage v6(const char* text) {
    struct sockaddr_storage ss;
    memset(&ss, 0, sizeof(ss));
    struct sockaddr_in6* a = (struct sockaddr_in6*)&ss;
    a->sin6_family = AF_INET6;
    inet_pton(AF_INET6, text, &a->sin6_addr);
    return ss;
}

/** Test one textual address against a list through the sockaddr path. */
static int allows(const IpAllowlist* list, const char* text, int family) {
    struct sockaddr_storage ss = (family == AF_INET) ? v4(text) : v6(text);
    return ip_allowlist_contains(list, (struct sockaddr*)&ss);
}

int main(void) {
    IpAllowlist list;

    printf("TEST 1: an empty list allows nothing\n");
    ip_allowlist_reset(&list);
    CHECK(ip_allowlist_is_empty(&list), "a reset list reports empty");
    CHECK(!allows(&list, "127.0.0.1", AF_INET), "loopback is not allowed by default");
    CHECK(!allows(&list, "::1", AF_INET6), "IPv6 loopback is not allowed by default");
    CHECK(!ip_allowlist_contains(&list, NULL), "a NULL address is refused");

    printf("\nTEST 2: a bare address is a host route\n");
    ip_allowlist_reset(&list);
    CHECK(ip_allowlist_add(&list, "10.0.0.4"), "a bare IPv4 address parses");
    CHECK(allows(&list, "10.0.0.4", AF_INET), "the exact address matches");
    CHECK(!allows(&list, "10.0.0.5", AF_INET), "its neighbour does not");
    CHECK(!allows(&list, "10.0.0.3", AF_INET), "its other neighbour does not");

    printf("\nTEST 3: a prefix matches its range and stops at the boundary\n");
    ip_allowlist_reset(&list);
    CHECK(ip_allowlist_add(&list, "10.1.2.0/24"), "a /24 parses");
    CHECK(allows(&list, "10.1.2.0", AF_INET),   "the network address matches");
    CHECK(allows(&list, "10.1.2.255", AF_INET), "the broadcast address matches");
    CHECK(!allows(&list, "10.1.3.0", AF_INET),  "the next /24 does not");
    CHECK(!allows(&list, "10.1.1.255", AF_INET),"the previous /24 does not");

    printf("\nTEST 4: prefixes that do not land on a byte boundary\n");
    ip_allowlist_reset(&list);
    CHECK(ip_allowlist_add(&list, "192.168.8.0/21"), "a /21 parses");
    CHECK(allows(&list, "192.168.8.1", AF_INET),  "the first address in range matches");
    CHECK(allows(&list, "192.168.15.254", AF_INET), "the last address in range matches");
    CHECK(!allows(&list, "192.168.16.1", AF_INET),  "one address past the range does not");
    CHECK(!allows(&list, "192.168.7.255", AF_INET), "one address before the range does not");

    printf("\nTEST 5: /0 matches everything, and is only reachable deliberately\n");
    ip_allowlist_reset(&list);
    CHECK(ip_allowlist_add(&list, "0.0.0.0/0"), "a /0 parses");
    CHECK(allows(&list, "8.8.8.8", AF_INET), "an arbitrary address matches /0");
    CHECK(!allows(&list, "::1", AF_INET6), "an IPv6 peer still does not match an IPv4 /0");

    printf("\nTEST 6: IPv6 rules and prefixes\n");
    ip_allowlist_reset(&list);
    CHECK(ip_allowlist_add(&list, "::1"), "IPv6 loopback parses");
    CHECK(ip_allowlist_add(&list, "fd00::/8"), "a ULA prefix parses");
    CHECK(allows(&list, "::1", AF_INET6), "IPv6 loopback matches");
    CHECK(allows(&list, "fd12:3456::1", AF_INET6), "an address inside the ULA prefix matches");
    CHECK(!allows(&list, "fe80::1", AF_INET6), "a link-local address does not");
    CHECK(!allows(&list, "::2", AF_INET6), "a near-miss loopback does not");

    printf("\nTEST 7: an IPv4-mapped peer is matched against IPv4 rules\n");
    ip_allowlist_reset(&list);
    CHECK(ip_allowlist_add(&list, "127.0.0.0/8"), "the loopback prefix parses");
    CHECK(allows(&list, "::ffff:127.0.0.1", AF_INET6),
          "a dual-stack listener's view of an IPv4 peer matches the IPv4 rule");
    CHECK(!allows(&list, "::ffff:10.0.0.1", AF_INET6),
          "a mapped address outside the rule still does not match");

    printf("\nTEST 8: malformed rules are refused, not stored loosely\n");
    ip_allowlist_reset(&list);
    CHECK(!ip_allowlist_add(&list, "10.0.0.1/33"),  "an IPv4 prefix over 32 is refused");
    CHECK(!ip_allowlist_add(&list, "::1/129"),      "an IPv6 prefix over 128 is refused");
    CHECK(!ip_allowlist_add(&list, "10.0.0.1/-1"),  "a negative prefix is refused");
    CHECK(!ip_allowlist_add(&list, "10.0.0.1/"),    "an empty prefix is refused");
    CHECK(!ip_allowlist_add(&list, "10.0.0.1/8x"),  "trailing junk after a prefix is refused");
    CHECK(!ip_allowlist_add(&list, "10.0.0.256"),   "an out-of-range octet is refused");
    CHECK(!ip_allowlist_add(&list, "not-an-address"), "arbitrary text is refused");
    CHECK(!ip_allowlist_add(&list, ""),             "an empty rule is refused");
    CHECK(ip_allowlist_is_empty(&list), "none of them was stored");

    printf("\nTEST 9: the list is bounded\n");
    ip_allowlist_reset(&list);
    for (int i = 0; i < IP_ALLOWLIST_MAX_RULES; i++)
        CHECK(ip_allowlist_add(&list, "10.0.0.1"), "a rule fits while there is room");
    CHECK(!ip_allowlist_add(&list, "10.0.0.2"), "one rule past capacity is refused");
    CHECK(!allows(&list, "10.0.0.2", AF_INET), "the refused rule has no effect");

    printf("\nTEST 10: the textual lookup agrees with the sockaddr lookup\n");
    ip_allowlist_reset(&list);
    ip_allowlist_add(&list, "172.16.0.0/12");
    CHECK(ip_allowlist_contains_text(&list, "172.16.5.9"), "an in-range address matches");
    CHECK(!ip_allowlist_contains_text(&list, "172.32.0.1"), "an out-of-range address does not");
    CHECK(!ip_allowlist_contains_text(&list, ""), "an empty string does not match");
    CHECK(!ip_allowlist_contains_text(&list, NULL), "NULL does not match");

    printf("\nTEST 11: the description names every rule\n");
    {
        char text[256];
        ip_allowlist_reset(&list);
        ip_allowlist_describe(&list, text, sizeof(text));
        CHECK(strstr(text, "empty") != NULL, "an empty list describes itself as empty");

        ip_allowlist_add(&list, "127.0.0.0/8");
        ip_allowlist_add(&list, "::1");
        ip_allowlist_describe(&list, text, sizeof(text));
        CHECK(strstr(text, "127.0.0.0/8") != NULL, "the IPv4 rule appears");
        CHECK(strstr(text, "::1/128") != NULL, "the IPv6 host route appears with its prefix");

        char tiny[8];
        ip_allowlist_describe(&list, tiny, sizeof(tiny));
        CHECK(strlen(tiny) < sizeof(tiny), "a short buffer is not overrun");
    }

    printf("\n");
    if (g_failures) {
        printf("%d ASSERTION%s FAILED\n", g_failures, g_failures == 1 ? "" : "S");
        return 1;
    }
    printf("ALL ASSERTIONS PASSED\n");
    return 0;
}
