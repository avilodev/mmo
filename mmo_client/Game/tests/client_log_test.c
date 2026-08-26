/**
 * @file
 * Check the client's rolling log file.
 *
 * The log exists so a player's disconnect leaves something behind, which means
 * the properties that matter are the unglamorous ones: the file is actually
 * written, a line survives a crash rather than sitting in a buffer, the level
 * filter works, and a busy session cannot fill a disk.
 */

#include "core/client_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
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

#define LOG_PATH "/tmp/mmo_client_log_test/client.log"

static long file_size(const char* path) {
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    return (long)st.st_size;
}

static int file_contains(const char* path, const char* needle) {
    FILE* f = fopen(path, "r");
    if (!f) return 0;

    char buf[8192];
    size_t got = fread(buf, 1, sizeof(buf) - 1, f);
    buf[got] = '\0';
    fclose(f);
    return strstr(buf, needle) != NULL;
}

static void clean(void) {
    remove(LOG_PATH);
    for (int i = 1; i <= 10; i++) {
        char rolled[256];
        snprintf(rolled, sizeof(rolled), "%s.%d", LOG_PATH, i);
        remove(rolled);
    }
}

int main(void) {
    printf("=== client log ===\n");

    clean();

    printf("\nTEST 1: lines reach the file, immediately\n");
    {
        setenv("MMO_CLIENT_LOG", LOG_PATH, 1);
        setenv("MMO_CLIENT_LOG_LEVEL", "info", 1);
        unsetenv("MMO_CLIENT_LOG_MAX_BYTES");
        unsetenv("MMO_CLIENT_LOG_KEEP");
        client_log_init();

        CHECK(file_size(LOG_PATH) >= 0, "the log file was created");
        CHECK(file_contains(LOG_PATH, "client log opened"),
              "and init announced itself in it");

        CLOG_INFO("a distinctive line %d", 4242);

        /* No close first: the whole point is that a line survives a process
         * that never gets to shut down cleanly. */
        CHECK(file_contains(LOG_PATH, "a distinctive line 4242"),
              "a line is on disk before the process exits");
        CHECK(file_contains(LOG_PATH, "client_log_test.c"),
              "with the file it came from");
    }

    printf("\nTEST 2: the level filter suppresses what it should\n");
    {
        client_log_close();
        clean();
        setenv("MMO_CLIENT_LOG_LEVEL", "warn", 1);
        client_log_init();

        CLOG_TRACE("trace-should-not-appear");
        CLOG_DEBUG("debug-should-not-appear");
        CLOG_INFO("info-should-not-appear");
        CLOG_WARN("warn-should-appear");
        CLOG_ERROR("error-should-appear");

        CHECK(!file_contains(LOG_PATH, "trace-should-not-appear"), "trace is dropped");
        CHECK(!file_contains(LOG_PATH, "debug-should-not-appear"), "debug is dropped");
        CHECK(!file_contains(LOG_PATH, "info-should-not-appear"),  "info is dropped");
        CHECK(file_contains(LOG_PATH, "warn-should-appear"),       "warn is kept");
        CHECK(file_contains(LOG_PATH, "error-should-appear"),      "error is kept");
        CHECK(client_log_level() == CLIENT_LOG_WARN, "and the level reads back");
    }

    printf("\nTEST 3: 'off' writes nothing at all\n");
    {
        client_log_close();
        clean();
        setenv("MMO_CLIENT_LOG_LEVEL", "off", 1);
        client_log_init();

        CLOG_ERROR("even-an-error-should-not-appear");
        CHECK(file_size(LOG_PATH) < 0, "no file is created when logging is off");
    }

    printf("\nTEST 4: the file rolls rather than growing forever\n");
    {
        client_log_close();
        clean();
        setenv("MMO_CLIENT_LOG_LEVEL", "info", 1);
        setenv("MMO_CLIENT_LOG_MAX_BYTES", "4096", 1);
        setenv("MMO_CLIENT_LOG_KEEP", "2", 1);
        client_log_init();

        /* Comfortably more than 2 x 4096 bytes, so at least two rolls happen
         * and the third-oldest file must have been dropped. */
        for (int i = 0; i < 400; i++)
            CLOG_INFO("a line of a fairly predictable length, number %d", i);

        CHECK(file_size(LOG_PATH) <= 8192,
              "the live file stays near its limit rather than growing");

        char rolled[256];
        snprintf(rolled, sizeof(rolled), "%s.1", LOG_PATH);
        CHECK(file_size(rolled) > 0, "a rolled file exists");

        snprintf(rolled, sizeof(rolled), "%s.3", LOG_PATH);
        CHECK(file_size(rolled) < 0,
              "and nothing beyond the configured number is kept");

        /* The most recent line must be in the live file: rotation must not
         * lose what it was in the middle of. */
        CHECK(file_contains(LOG_PATH, "number 399"),
              "the newest line is in the live file");
    }

    printf("\nTEST 5: an unwritable path is survivable\n");
    {
        client_log_close();
        setenv("MMO_CLIENT_LOG", "/proc/definitely/not/writable/client.log", 1);
        client_log_init();

        /* The assertion is only that this returns: a client that will not
         * start because it could not write a log is worse than one with no
         * log. */
        CLOG_ERROR("this goes nowhere and that is fine");
        CHECK(1, "logging to an unwritable path does not crash");
        client_log_close();
    }

    clean();
    rmdir("/tmp/mmo_client_log_test");

    printf("\n");
    if (g_failures) {
        printf("%d ASSERTION%s FAILED\n", g_failures, g_failures == 1 ? "" : "S");
        return 1;
    }
    printf("ALL ASSERTIONS PASSED\n");
    return 0;
}
