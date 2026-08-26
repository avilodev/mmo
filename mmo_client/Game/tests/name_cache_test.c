/**
 * @file
 * Check the character-name cache: what it asks for, once, and what it keeps.
 *
 * The cache exists so nearby players have names without the position
 * broadcast carrying 32 constant bytes per player twenty times a second. That
 * trade only holds if the asking is genuinely rare, so what is checked here is
 * mostly restraint: an identifier is asked about once and not again, a batch
 * of strangers is one question rather than thirty, and an identifier that
 * never resolves does not turn into a request per frame.
 */

#include "network/name_cache.h"
#include "net_internal.h"

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

/* The cache sends through g_net; this test owns it and never connects, so
 * flushing clears the queue without a socket being involved. */
#define NET_STUBS_OWN_CONTEXT
#include "hostcompat/net_stubs.c"

int main(void) {
    printf("=== client name cache ===\n");

    memset(&g_net, 0, sizeof(g_net));
    InitializeCriticalSection(&g_net.response_lock);
    g_net.socket = -1;

    printf("\nTEST 1: an unknown identifier is asked about, once\n");
    {
        name_cache_reset();

        const char* first = name_cache_lookup(1001);
        CHECK(first != NULL, "a lookup always returns something to draw");
        CHECK(strstr(first, "1001") == NULL,
              "and it is not the identifier dressed up as a name");
        CHECK(name_cache_pending_count() == 1, "the identifier is queued once");

        name_cache_lookup(1001);
        name_cache_lookup(1001);
        CHECK(name_cache_pending_count() == 1,
              "looking it up again does not queue it again");
    }

    printf("\nTEST 2: a crowd is one question, not thirty\n");
    {
        name_cache_reset();
        for (uint32_t id = 2000; id < 2030; id++) name_cache_lookup(id);

        CHECK(name_cache_pending_count() == 30, "thirty strangers, thirty ids");

        /* Not connected, so the flush drops the batch rather than sending it.
         * What matters here is that the queue is one batch and is cleared. */
        name_cache_flush_requests();
        CHECK(name_cache_pending_count() == 0, "one flush clears the whole batch");
    }

    printf("\nTEST 3: the queue cannot outgrow what one packet carries\n");
    {
        name_cache_reset();
        for (uint32_t id = 3000; id < 3000 + 200; id++) name_cache_lookup(id);
        CHECK(name_cache_pending_count() <= MAX_NAME_QUERY,
              "a very large crowd still queues at most one packet's worth");
    }

    printf("\nTEST 4: an answered identifier is answered from memory\n");
    {
        name_cache_reset();
        name_cache_lookup(4001);
        name_cache_flush_requests();

        name_cache_store(4001, "Bramblefoot");
        CHECK(name_cache_known_count() == 1, "the name is held");
        CHECK(strcmp(name_cache_lookup(4001), "Bramblefoot") == 0,
              "and returned");
        CHECK(name_cache_pending_count() == 0,
              "a known identifier is never asked about again");
    }

    printf("\nTEST 5: an unanswered identifier does not ask every frame\n");
    {
        /* The failure this guards against: a character seen and then logged
         * out never resolves, so without a retry window the client would
         * re-queue them on every single lookup -- sixty requests a second, for
         * someone who is not there. */
        name_cache_reset();
        name_cache_lookup(5001);
        name_cache_flush_requests();

        for (int i = 0; i < 1000; i++) name_cache_lookup(5001);
        CHECK(name_cache_pending_count() == 0,
              "a thousand lookups of an unanswered id queue nothing more");
    }

    printf("\nTEST 6: a hostile or empty answer is ignored\n");
    {
        name_cache_reset();
        name_cache_store(0, "nobody");
        CHECK(name_cache_known_count() == 0, "identifier zero is not cached");

        name_cache_store(6001, "");
        CHECK(name_cache_known_count() == 0, "an empty name is not cached");

        name_cache_store(6002, NULL);
        CHECK(name_cache_known_count() == 0, "a NULL name is not cached");
    }

    printf("\nTEST 7: the table recycles rather than filling up\n");
    {
        name_cache_reset();
        for (uint32_t id = 1; id <= NAME_CACHE_CAPACITY + 50; id++) {
            char name[32];
            snprintf(name, sizeof(name), "Char%u", id);
            name_cache_store(id, name);
        }
        CHECK(name_cache_known_count() <= NAME_CACHE_CAPACITY,
              "the table never exceeds its capacity");

        /* The most recently stored must survive: those are the ones on screen. */
        char expected[32];
        snprintf(expected, sizeof(expected), "Char%u", NAME_CACHE_CAPACITY + 50);
        CHECK(strcmp(name_cache_lookup(NAME_CACHE_CAPACITY + 50), expected) == 0,
              "and keeps the most recent entries");
    }

    DeleteCriticalSection(&g_net.response_lock);

    printf("\n");
    if (g_failures) {
        printf("%d ASSERTION%s FAILED\n", g_failures, g_failures == 1 ? "" : "S");
        return 1;
    }
    printf("ALL ASSERTIONS PASSED\n");
    return 0;
}
