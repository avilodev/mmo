/**
 * @file
 * Check the correlation id that ties a player's login across three services.
 *
 * A login crosses the login server, the realm and a world, and until this
 * existed nothing joined their log lines: account_id appeared in some,
 * character_id in others, and following one player through a failed login
 * meant lining up timestamps across three files.
 *
 * The properties that make it worth having are the ones checked here: an id
 * appears on every line a thread emits while carrying it, it does not appear
 * when the thread is not, it does not leak from one thread to another, and it
 * can be cleared -- because a *wrong* id, attributing one player's lines to
 * another, is worse than no id at all.
 */

#include "log.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

#define LOG_PATH "/tmp/mmo_log_trace_test.log"

static int file_contains(const char* needle) {
    FILE* f = fopen(LOG_PATH, "r");
    if (!f) return 0;
    char buf[65536];
    size_t got = fread(buf, 1, sizeof(buf) - 1, f);
    buf[got] = '\0';
    fclose(f);
    return strstr(buf, needle) != NULL;
}

static void truncate_log(void) {
    FILE* f = fopen(LOG_PATH, "w");
    if (f) fclose(f);
}

/** What one thread reports about the id it saw. */
typedef struct {
    char adopted[TRACE_ID_LEN];
    char observed[TRACE_ID_LEN];
} ThreadResult;

static void* thread_body(void* arg) {
    ThreadResult* r = arg;

    /* A brand-new thread must start with no id. If the storage were process-
     * wide rather than per thread, this would inherit whatever main set. */
    snprintf(r->observed, sizeof(r->observed), "%s", log_get_trace());

    log_set_trace(r->adopted);
    usleep(20000);   /* overlap with the other threads */

    /* And must still be carrying its own after the others have set theirs. */
    if (strcmp(log_get_trace(), r->adopted) != 0)
        snprintf(r->observed, sizeof(r->observed), "STOLEN");

    log_clear_trace();
    return NULL;
}

int main(void) {
    /* The file sink is how the emitted lines are inspected. Console output is
     * turned off so the test's own output stays readable. */
    setenv("MMO_LOG_FILE", LOG_PATH, 1);
    setenv("MMO_LOG_CONSOLE", "0", 1);
    setenv("MMO_LOG_LEVEL", "info", 1);
    remove(LOG_PATH);

    log_init();
    printf("=== log correlation ===\n");

    printf("\nTEST 1: an id is generated, and looks like one\n");
    {
        char a[TRACE_ID_LEN], b[TRACE_ID_LEN];
        log_new_trace(a);
        log_new_trace(b);

        CHECK(strlen(a) == TRACE_ID_LEN - 1, "an id is the documented length");
        CHECK(strcmp(a, b) != 0, "two ids differ");

        int hex_only = 1;
        for (const char* p = a; *p; p++) {
            if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f'))) hex_only = 0;
        }
        CHECK(hex_only, "and it is hex, so it survives a grep and a paste");
    }

    printf("\nTEST 2: a carried id appears on every line\n");
    {
        truncate_log();
        log_set_trace("abcdef0123456789");

        LOG_INFO("first line under the id");
        LOG_WARN("second line under the id");

        CHECK(file_contains("abcdef0123456789"), "the id reaches the log");
        CHECK(file_contains("first line under the id"), "with the first message");
        CHECK(file_contains("second line under the id"), "and the second");
    }

    printf("\nTEST 3: clearing it stops the attribution\n");
    {
        truncate_log();
        log_clear_trace();

        CHECK(strcmp(log_get_trace(), "") == 0, "the thread carries nothing");
        LOG_INFO("a line belonging to nobody");

        CHECK(file_contains("a line belonging to nobody"), "the line is still logged");
        CHECK(!file_contains("abcdef0123456789"),
              "but the previous player's id is not on it");
    }

    printf("\nTEST 4: two players do not become one\n");
    {
        truncate_log();

        log_set_trace("1111111111111111");
        LOG_INFO("work for the first player");
        log_clear_trace();

        log_set_trace("2222222222222222");
        LOG_INFO("work for the second player");
        log_clear_trace();

        /* The bug this guards against is a pooled worker keeping the id of the
         * player it handled last, which silently files one player's failures
         * under another's login. */
        FILE* f = fopen(LOG_PATH, "r");
        CHECK(f != NULL, "the log can be read back");
        int first_correct = 0, second_correct = 0, crossed = 0;
        if (f) {
            char line[1024];
            while (fgets(line, sizeof(line), f)) {
                int has_first  = strstr(line, "1111111111111111") != NULL;
                int has_second = strstr(line, "2222222222222222") != NULL;
                if (strstr(line, "work for the first player")) {
                    if (has_first) first_correct = 1;
                    if (has_second) crossed = 1;
                }
                if (strstr(line, "work for the second player")) {
                    if (has_second) second_correct = 1;
                    if (has_first) crossed = 1;
                }
            }
            fclose(f);
        }
        CHECK(first_correct,  "the first player's line carries the first id");
        CHECK(second_correct, "the second player's line carries the second id");
        CHECK(!crossed, "and neither line carries the other's");
    }

    printf("\nTEST 5: the id is per thread, not per process\n");
    {
        enum { THREADS = 8 };
        pthread_t threads[THREADS];
        ThreadResult results[THREADS];

        log_set_trace("mainmainmainmain");

        for (int i = 0; i < THREADS; i++) {
            memset(&results[i], 0, sizeof(results[i]));
            snprintf(results[i].adopted, sizeof(results[i].adopted),
                     "thread%010d", i);
            pthread_create(&threads[i], NULL, thread_body, &results[i]);
        }
        for (int i = 0; i < THREADS; i++) pthread_join(threads[i], NULL);

        int inherited = 0, stolen = 0;
        for (int i = 0; i < THREADS; i++) {
            if (strcmp(results[i].observed, "STOLEN") == 0) stolen++;
            else if (results[i].observed[0] != '\0')        inherited++;
        }

        CHECK(inherited == 0, "no thread started with another's id");
        CHECK(stolen == 0, "and none had its own replaced while it worked");
        CHECK(strcmp(log_get_trace(), "mainmainmainmain") == 0,
              "the main thread kept its own throughout");

        log_clear_trace();
    }

    printf("\nTEST 6: an over-long id is truncated, not overflowed\n");
    {
        char huge[512];
        memset(huge, 'x', sizeof(huge) - 1);
        huge[sizeof(huge) - 1] = '\0';

        log_set_trace(huge);
        CHECK(strlen(log_get_trace()) == TRACE_ID_LEN - 1,
              "a hostile id is cut to the field it fits in");
        log_clear_trace();

        log_set_trace(NULL);
        CHECK(strcmp(log_get_trace(), "") == 0, "and NULL clears rather than crashes");
    }

    log_close();
    remove(LOG_PATH);

    printf("\n");
    if (g_failures) {
        printf("%d ASSERTION%s FAILED\n", g_failures, g_failures == 1 ? "" : "S");
        return 1;
    }
    printf("ALL ASSERTIONS PASSED\n");
    return 0;
}
