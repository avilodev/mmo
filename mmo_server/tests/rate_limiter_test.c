/**
 * @file
 * Check that the login rate limiter counts the right thing and never gives up.
 *
 * Two properties this suite exists to hold. An IPv6 attacker must not be able
 * to walk out of a budget by changing the low bits of an address they were
 * handed a whole allocation of. And a crowded table must grow rather than fail
 * closed, because failing closed refuses service to whoever happened to hash
 * nearby.
 */

#include "rate_limiter.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
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

/** Resolve one address to its bucket key. */
static const char* bucket(const char* ip) {
    static char key[RL_KEY_MAXLEN];
    key[0] = '\0';
    rate_limiter_bucket_key(ip, key, sizeof(key));
    return key;
}

/** Every address inside one /64 must resolve to the same bucket. */
static void test_ipv6_shares_a_prefix_bucket(void) {
    printf("IPv6 addresses are counted per /64\n");

    char first[RL_KEY_MAXLEN];
    snprintf(first, sizeof(first), "%s", bucket("2001:db8:1:2::1"));

    CHECK(strcmp(first, bucket("2001:db8:1:2::2")) == 0,
          "a second address in the same /64 shares the bucket");
    CHECK(strcmp(first, bucket("2001:db8:1:2:ffff:ffff:ffff:ffff")) == 0,
          "so does the last address in that /64");
    CHECK(strcmp(first, bucket("2001:db8:1:3::1")) != 0,
          "an address in the next /64 does not");
    CHECK(strstr(first, "/64") != NULL, "and the key records the prefix length");
}

/** An IPv4 address is its own bucket, however the socket reports it. */
static void test_ipv4_bucketing(void) {
    printf("IPv4 bucketing\n");

    CHECK(strcmp(bucket("203.0.113.7"), "203.0.113.7") == 0,
          "an IPv4 address is its own key");
    CHECK(strcmp(bucket("203.0.113.8"), "203.0.113.7") != 0,
          "a different IPv4 address is a different key");
    CHECK(strcmp(bucket("::ffff:203.0.113.7"), "203.0.113.7") == 0,
          "an IPv4-mapped IPv6 address maps to the same key as the IPv4 form");
}

/** An address the limiter cannot parse yields no key, so callers fail closed. */
static void test_unusable_addresses(void) {
    printf("unusable addresses\n");

    char key[RL_KEY_MAXLEN];
    CHECK(!rate_limiter_bucket_key(NULL, key, sizeof(key)), "NULL yields no key");
    CHECK(!rate_limiter_bucket_key("", key, sizeof(key)), "an empty string yields no key");
    CHECK(!rate_limiter_bucket_key("not-an-address", key, sizeof(key)),
          "garbage yields no key");

    CHECK(rate_limiter_check(NULL) != 0, "an unidentifiable peer is refused");
    CHECK(rate_limiter_check("") != 0, "so is one with no address");
    CHECK(rate_limiter_check("nonsense") != 0, "so is one with an unparseable address");
}

/** The failure budget applies to the whole /64, not to each address in it. */
static void test_ipv6_cannot_walk_out_of_the_budget(void) {
    printf("an IPv6 /64 shares one failure budget\n");
    rate_limiter_init();

    /* One failure from each of many distinct addresses inside one allocation.
     * Per /128 this is one failure each and blocks nobody; per /64 it is the
     * same attacker exhausting one budget. */
    int blocked_at = -1;
    for (int i = 0; i < 64 && blocked_at < 0; i++) {
        char ip[64];
        snprintf(ip, sizeof(ip), "2001:db8:aaaa:bbbb::%x", i + 1);
        if (rate_limiter_record_failure(ip)) blocked_at = i;
    }

    CHECK(blocked_at >= 0,
          "rotating the low bits of one /64 still runs into the failure budget");

    /* And a fresh address in the same /64 is refused on sight. */
    CHECK(rate_limiter_check("2001:db8:aaaa:bbbb::dead") != 0,
          "an address never seen before in that /64 is already blocked");

    /* A neighbouring allocation is untouched. */
    CHECK(rate_limiter_check("2001:db8:aaaa:cccc::1") == 0,
          "the adjacent /64 is unaffected");
}

/** The connection budget is likewise per bucket. */
static void test_connection_budget_is_per_bucket(void) {
    printf("connection budget\n");
    rate_limiter_init();

    int refused_at = -1;
    for (int i = 0; i < 200 && refused_at < 0; i++) {
        char ip[64];
        snprintf(ip, sizeof(ip), "2001:db8:c0de::%x", i + 1);
        if (rate_limiter_record_connection(ip)) refused_at = i;
    }

    CHECK(refused_at >= 0, "connection churn across one /64 hits the budget");
    CHECK(rate_limiter_check("198.51.100.1") == 0,
          "an unrelated IPv4 address is unaffected");
}

/** The table grows instead of refusing addresses it cannot place. */
static void test_table_grows_rather_than_failing_closed(void) {
    printf("table growth\n");
    rate_limiter_init();

    size_t initial = rate_limiter_capacity();
    CHECK(initial > 0, "the table is allocated at init");

    /* Far more distinct addresses than the fixed table used to hold. Each one
     * gets a single connection, well inside its own budget, so none of them
     * should be refused -- and the old code refused whatever it could not
     * place. */
    const int addresses = 40000;
    int refused = 0;
    for (int i = 0; i < addresses; i++) {
        char ip[64];
        snprintf(ip, sizeof(ip), "10.%d.%d.%d",
                 (i >> 16) & 0xff, (i >> 8) & 0xff, i & 0xff);
        if (rate_limiter_record_connection(ip)) refused++;
    }

    /* 10.x.y.z repeats every 65536, so some addresses genuinely recur; with
     * 40000 of them none repeats, and no bucket can reach its budget. */
    CHECK(refused == 0, "no address was refused for want of a table slot");
    CHECK(rate_limiter_capacity() > initial, "the table grew to hold them");
    CHECK(rate_limiter_tracked() > (size_t)addresses / 2,
          "and it is actually tracking them");
}

/** Registrations are budgeted per address, independently of failures. */
static void test_registration_budget(void) {
    printf("registration budget\n");
    rate_limiter_init();

    /* Every one of these "succeeds" as far as the limiter is concerned -- no
     * failure is ever recorded -- which is exactly the shape that used to be
     * unbounded: a unique username always worked, so the failure counter never
     * moved and nothing else was watching. */
    int allowed = 0, refused_at = -1;
    for (int i = 0; i < 20; i++) {
        if (rate_limiter_record_registration("203.0.113.44")) {
            refused_at = i;
            break;
        }
        allowed++;
    }

    CHECK(refused_at >= 0, "an address runs out of registration budget");
    CHECK(allowed > 0, "but the first few are allowed");
    CHECK(rate_limiter_record_registration("203.0.113.45") == 0,
          "a different address keeps its own budget");

    /* Being out of registration budget must not lock the address out of
     * logging in to accounts it already has. */
    CHECK(rate_limiter_check("203.0.113.44") == 0,
          "an exhausted registration budget does not block the address");

    /* An IPv6 /64 is one bucket here too, so an attacker with a routed prefix
     * cannot walk out of the limit one address at a time. */
    int walked = 0;
    for (int i = 0; i < 40; i++) {
        char ip[64];
        snprintf(ip, sizeof(ip), "2001:db8:7e57::%x", i + 1);
        if (rate_limiter_record_registration(ip)) { walked = 1; break; }
    }
    CHECK(walked, "walking an IPv6 /64 hits the same registration budget");
}

/** The registration budget is configurable. */
static void test_registration_budget_is_configurable(void) {
    printf("configurable registration budget\n");

    setenv("MMO_LOGIN_MAX_REGISTRATIONS_PER_HOUR", "1", 1);
    rate_limiter_init();

    CHECK(rate_limiter_record_registration("198.51.100.9") == 0,
          "the first registration fits in a budget of one");
    CHECK(rate_limiter_record_registration("198.51.100.9") == 1,
          "the second does not");

    unsetenv("MMO_LOGIN_MAX_REGISTRATIONS_PER_HOUR");
    rate_limiter_init();

    CHECK(rate_limiter_record_registration("198.51.100.9") == 0,
          "and the default is back after the override is removed");
    CHECK(rate_limiter_record_registration("198.51.100.9") == 0,
          "with room for more than one");
}

/** A configured prefix changes the bucket width. */
static void test_prefix_is_configurable(void) {
    printf("configurable prefix\n");

    setenv("MMO_LOGIN_IPV6_PREFIX", "48", 1);
    rate_limiter_init();

    char wide[RL_KEY_MAXLEN];
    snprintf(wide, sizeof(wide), "%s", bucket("2001:db8:9999:1::1"));
    CHECK(strcmp(wide, bucket("2001:db8:9999:2::1")) == 0,
          "at /48 two different /64s share a bucket");
    CHECK(strstr(wide, "/48") != NULL, "and the key records /48");

    unsetenv("MMO_LOGIN_IPV6_PREFIX");
    rate_limiter_init();

    /* Copied out: bucket() answers into one static buffer, so comparing two of
     * its return values directly compares a string with itself. */
    char narrow[RL_KEY_MAXLEN];
    snprintf(narrow, sizeof(narrow), "%s", bucket("2001:db8:9999:1::1"));
    CHECK(strcmp(narrow, bucket("2001:db8:9999:2::1")) != 0,
          "back at the default they are separate again");
}

int main(void) {
    log_init();
    rate_limiter_init();

    test_ipv6_shares_a_prefix_bucket();
    test_ipv4_bucketing();
    test_unusable_addresses();
    test_ipv6_cannot_walk_out_of_the_budget();
    test_connection_budget_is_per_bucket();
    test_table_grows_rather_than_failing_closed();
    test_registration_budget();
    test_registration_budget_is_configurable();
    test_prefix_is_configurable();

    if (g_failures) {
        printf("\n%d rate limiter check(s) FAILED\n", g_failures);
        return 1;
    }
    printf("\nAll rate limiter checks passed\n");
    return 0;
}
