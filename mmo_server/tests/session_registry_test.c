/**
 * @file
 * Check descriptor-indexed session lookups, stale-login kicks, and index churn.
 *
 * The registry used to be a linear scan over a 10,000-entry array; the identifier
 * lookups are now open-addressed indexes, so these tests care most about the cases
 * that can corrupt an index: rebinding, tombstone accumulation, and hash collisions.
 */

#include "session_registry.h"

#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>

/** Descriptors below this are the process's own std streams; never register them. */
#define FIRST_TEST_FD 64

/** Assert that every lookup route agrees a session exists with these identifiers. */
static void assert_present(int fd, uint32_t account_id, uint32_t character_id) {
    SessionEntry e;

    memset(&e, 0, sizeof(e));
    assert(session_find_by_fd(fd, &e) == 1);
    assert(e.fd == fd && e.account_id == account_id && e.character_id == character_id);
    assert(e.active);

    memset(&e, 0, sizeof(e));
    assert(session_find_by_account(account_id, &e) == 1);
    assert(e.fd == fd && e.character_id == character_id);

    memset(&e, 0, sizeof(e));
    assert(session_find_by_character(character_id, &e) == 1);
    assert(e.fd == fd && e.account_id == account_id);
}

/** Assert that no lookup route can reach these identifiers. */
static void assert_absent(int fd, uint32_t account_id, uint32_t character_id) {
    assert(session_find_by_fd(fd, NULL) == 0);
    assert(session_find_by_account(account_id, NULL) == 0);
    assert(session_find_by_character(character_id, NULL) == 0);
}

static void test_uninitialised_fails_closed(void) {
    printf("TEST 1: lookups before init fail closed\n");

    assert(session_find_by_fd(FIRST_TEST_FD, NULL) == 0);
    assert(session_find_by_account(1, NULL) == 0);
    assert(session_find_by_character(1, NULL) == 0);
    session_registry_remove(FIRST_TEST_FD);        // must not crash

    printf("  no session is reported and removal is a no-op\n");
}

static void test_add_find_remove(void) {
    printf("\nTEST 2: a session is reachable by descriptor, account, and character\n");

    assert(session_registry_add(FIRST_TEST_FD, 1000, 2000) == 0);
    assert_present(FIRST_TEST_FD, 1000, 2000);
    printf("  all three lookups agree after add\n");

    session_registry_remove(FIRST_TEST_FD);
    assert_absent(FIRST_TEST_FD, 1000, 2000);
    printf("  all three lookups are cleared after remove\n");

    // Removing twice must not resurrect anything or corrupt the indexes.
    session_registry_remove(FIRST_TEST_FD);
    assert_absent(FIRST_TEST_FD, 1000, 2000);
    printf("  a repeated remove is harmless\n");
}

static void test_out_of_range_descriptor(void) {
    printf("\nTEST 3: an out-of-range descriptor is refused\n");

    assert(session_registry_add(-1, 1, 1) == -1);
    assert(session_registry_add(1 << 30, 1, 1) == -1);
    assert(session_find_by_account(1, NULL) == 0);
    printf("  negative and oversized descriptors are rejected without indexing\n");
}

static void test_stale_login_is_kicked(void) {
    printf("\nTEST 4: a second login for one account kicks the first\n");

    // Real sockets: the kick path calls shutdown() on the descriptor it evicts.
    int pair[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    int old_fd = pair[0];
    int new_fd = pair[1];

    assert(session_registry_add(old_fd, 5000, 6000) == 0);
    assert_present(old_fd, 5000, 6000);

    assert(session_registry_add(new_fd, 5000, 6000) == 0);

    // The old descriptor keeps no session, and every lookup now resolves to the new one.
    assert(session_find_by_fd(old_fd, NULL) == 0);
    assert_present(new_fd, 5000, 6000);
    printf("  the old descriptor is evicted and lookups follow the new one\n");

    session_registry_remove(new_fd);
    close(pair[0]);
    close(pair[1]);
}

static void test_reauth_on_same_descriptor_rebinds(void) {
    printf("\nTEST 5: re-authenticating a descriptor drops its old identifiers\n");

    int fd = FIRST_TEST_FD + 1;
    assert(session_registry_add(fd, 7000, 8000) == 0);
    assert(session_registry_add(fd, 7001, 8001) == 0);

    // The stale identifiers must not still point at this descriptor.
    assert(session_find_by_account(7000, NULL) == 0);
    assert(session_find_by_character(8000, NULL) == 0);
    assert_present(fd, 7001, 8001);
    printf("  the superseded account and character no longer resolve\n");

    session_registry_remove(fd);
}

static void test_many_sessions_and_churn(void) {
    printf("\nTEST 6: many sessions coexist and survive login/logout churn\n");

    const int n = 400;

    for (int i = 0; i < n; i++)
        assert(session_registry_add(FIRST_TEST_FD + i, 10000u + (uint32_t)i,
                                    20000u + (uint32_t)i) == 0);

    for (int i = 0; i < n; i++)
        assert_present(FIRST_TEST_FD + i, 10000u + (uint32_t)i, 20000u + (uint32_t)i);
    printf("  %d concurrent sessions all resolve correctly\n", n);

    // Drop every other session; the survivors must still be reachable past the
    // tombstones the removals leave in the probe chains.
    for (int i = 0; i < n; i += 2)
        session_registry_remove(FIRST_TEST_FD + i);

    for (int i = 0; i < n; i++) {
        if (i % 2 == 0)
            assert_absent(FIRST_TEST_FD + i, 10000u + (uint32_t)i, 20000u + (uint32_t)i);
        else
            assert_present(FIRST_TEST_FD + i, 10000u + (uint32_t)i, 20000u + (uint32_t)i);
    }
    printf("  survivors remain reachable through the tombstoned chains\n");

    /* Churn hard enough to cross the rebuild threshold, then re-verify. A rebuild
     * that lost or duplicated an entry would show up here. */
    for (int round = 0; round < 200; round++) {
        for (int i = 0; i < n; i += 2) {
            assert(session_registry_add(FIRST_TEST_FD + i, 10000u + (uint32_t)i,
                                        20000u + (uint32_t)i) == 0);
            session_registry_remove(FIRST_TEST_FD + i);
        }
    }

    for (int i = 1; i < n; i += 2)
        assert_present(FIRST_TEST_FD + i, 10000u + (uint32_t)i, 20000u + (uint32_t)i);
    printf("  index rebuilds preserve every untouched session\n");

    for (int i = 1; i < n; i += 2)
        session_registry_remove(FIRST_TEST_FD + i);
    for (int i = 0; i < n; i++)
        assert_absent(FIRST_TEST_FD + i, 10000u + (uint32_t)i, 20000u + (uint32_t)i);
    printf("  the registry drains completely\n");
}

static void test_colliding_identifiers(void) {
    printf("\nTEST 7: identifiers that collide in the hash stay distinct\n");

    /* The index hashes with a multiplicative constant and probes linearly, so keys
     * spaced by a power of two are the ones most likely to share a bucket. */
    const int n = 64;
    for (int i = 0; i < n; i++) {
        uint32_t account = 1u + ((uint32_t)i << 16);
        uint32_t chr     = 2u + ((uint32_t)i << 16);
        assert(session_registry_add(FIRST_TEST_FD + i, account, chr) == 0);
    }

    for (int i = 0; i < n; i++) {
        uint32_t account = 1u + ((uint32_t)i << 16);
        uint32_t chr     = 2u + ((uint32_t)i << 16);
        assert_present(FIRST_TEST_FD + i, account, chr);
    }
    printf("  %d colliding keys each resolve to their own descriptor\n", n);

    for (int i = 0; i < n; i++) session_registry_remove(FIRST_TEST_FD + i);
}

/** Exercise the registry from several threads at once. */
#define RACE_THREADS  8
#define RACE_ROUNDS   2000

static atomic_int g_race_failures = 0;

static void* race_worker(void* arg) {
    int id = *(int*)arg;
    int fd = FIRST_TEST_FD + id;
    uint32_t account = 90000u + (uint32_t)id;
    uint32_t chr     = 91000u + (uint32_t)id;

    for (int r = 0; r < RACE_ROUNDS; r++) {
        if (session_registry_add(fd, account, chr) != 0) {
            atomic_fetch_add(&g_race_failures, 1);
            continue;
        }

        /* Each thread owns its own descriptor and identifiers, so its own view must
         * be exact no matter what the other threads are doing to the shared table. */
        SessionEntry e;
        if (!session_find_by_fd(fd, &e) || e.account_id != account || e.character_id != chr)
            atomic_fetch_add(&g_race_failures, 1);
        if (!session_find_by_account(account, &e) || e.fd != fd)
            atomic_fetch_add(&g_race_failures, 1);
        if (!session_find_by_character(chr, &e) || e.fd != fd)
            atomic_fetch_add(&g_race_failures, 1);

        session_registry_remove(fd);
    }
    return NULL;
}

static void test_concurrent_access(void) {
    printf("\nTEST 8: concurrent logins and lookups stay consistent\n");

    pthread_t threads[RACE_THREADS];
    int ids[RACE_THREADS];

    for (int i = 0; i < RACE_THREADS; i++) {
        ids[i] = i;
        assert(pthread_create(&threads[i], NULL, race_worker, &ids[i]) == 0);
    }
    for (int i = 0; i < RACE_THREADS; i++) pthread_join(threads[i], NULL);

    assert(atomic_load(&g_race_failures) == 0);
    printf("  %d threads x %d add/find/remove rounds, 0 inconsistencies\n",
           RACE_THREADS, RACE_ROUNDS);
}

int main(void) {
    test_uninitialised_fails_closed();

    session_registry_init();

    test_add_find_remove();
    test_out_of_range_descriptor();
    test_stale_login_is_kicked();
    test_reauth_on_same_descriptor_rebinds();
    test_many_sessions_and_churn();
    test_colliding_identifiers();
    test_concurrent_access();

    session_registry_shutdown();

    // After shutdown the registry must fail closed again rather than read freed memory.
    assert(session_find_by_fd(FIRST_TEST_FD, NULL) == 0);
    assert(session_find_by_account(1000, NULL) == 0);

    printf("\nALL ASSERTIONS PASSED\n");
    return 0;
}
