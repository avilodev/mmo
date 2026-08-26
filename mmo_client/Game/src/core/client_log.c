/**
 * @file
 * The client's rolling log file.
 *
 * Deliberately the same shape as the server's common/src/log.c -- same level
 * names, same timestamp format, same rotation scheme -- because a player's
 * report and an operator's investigation should read as two halves of one
 * story rather than two unrelated formats.
 */

#include "core/client_log.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

#ifdef _WIN32
  #include <windows.h>
  #include <direct.h>
  #define log_mkdir(path) _mkdir(path)
  static CRITICAL_SECTION g_lock;
  static int g_lock_ready = 0;
  #define LOCK_INIT()   do { if (!g_lock_ready) { InitializeCriticalSection(&g_lock); g_lock_ready = 1; } } while (0)
  #define LOCK()        do { if (g_lock_ready) EnterCriticalSection(&g_lock); } while (0)
  #define UNLOCK()      do { if (g_lock_ready) LeaveCriticalSection(&g_lock); } while (0)
  #define LOCK_DESTROY() do { if (g_lock_ready) { DeleteCriticalSection(&g_lock); g_lock_ready = 0; } } while (0)
#else
  #include <pthread.h>
  #define log_mkdir(path) mkdir((path), 0755)
  static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
  #define LOCK_INIT()    ((void)0)
  #define LOCK()         pthread_mutex_lock(&g_lock)
  #define UNLOCK()       pthread_mutex_unlock(&g_lock)
  #define LOCK_DESTROY() ((void)0)
#endif

#define DEFAULT_PATH      "Game/logs/client.log"
#define DEFAULT_MAX_BYTES (4L * 1024 * 1024)
#define DEFAULT_KEEP      3

static FILE*          g_file       = NULL;
static char           g_path[512]  = DEFAULT_PATH;
static long           g_max_bytes  = DEFAULT_MAX_BYTES;
static int            g_keep       = DEFAULT_KEEP;
static long           g_bytes      = 0;
static ClientLogLevel g_level      = CLIENT_LOG_INFO;

static const char* const LEVEL_TAG[] = {
    "TRACE", "DEBUG", "INFO ", "WARN ", "ERROR"
};

static ClientLogLevel parse_level(const char* s) {
    if (!s || !*s)                    return CLIENT_LOG_INFO;
    if (strcmp(s, "trace") == 0)      return CLIENT_LOG_TRACE;
    if (strcmp(s, "debug") == 0)      return CLIENT_LOG_DEBUG;
    if (strcmp(s, "info")  == 0)      return CLIENT_LOG_INFO;
    if (strcmp(s, "warn")  == 0)      return CLIENT_LOG_WARN;
    if (strcmp(s, "error") == 0)      return CLIENT_LOG_ERROR;
    if (strcmp(s, "off")   == 0)      return CLIENT_LOG_OFF;
    return CLIENT_LOG_INFO;
}

/** Create the directory a path lives in. Best effort. */
static void ensure_directory(const char* path) {
    char dir[512];
    snprintf(dir, sizeof(dir), "%s", path);

    char* last = strrchr(dir, '/');
#ifdef _WIN32
    char* back = strrchr(dir, '\\');
    if (back && (!last || back > last)) last = back;
#endif
    if (!last) return;
    *last = '\0';
    if (dir[0]) log_mkdir(dir);
}

/** Roll <path>.N-1 to <path>.N and the live file to <path>.1. Caller holds the lock. */
static void rotate_locked(void) {
    if (!g_file) return;

    fclose(g_file);
    g_file = NULL;

    char from[600], to[600];

    /* Drop the oldest, then shift each survivor up one. */
    snprintf(to, sizeof(to), "%s.%d", g_path, g_keep);
    remove(to);
    for (int i = g_keep - 1; i >= 1; i--) {
        snprintf(from, sizeof(from), "%s.%d", g_path, i);
        snprintf(to,   sizeof(to),   "%s.%d", g_path, i + 1);
        rename(from, to);
    }
    snprintf(to, sizeof(to), "%s.1", g_path);
    rename(g_path, to);

    g_file  = fopen(g_path, "a");
    g_bytes = 0;
}

void client_log_init(void) {
    LOCK_INIT();
    LOCK();

    if (g_file) { UNLOCK(); return; }

    const char* configured = getenv("MMO_CLIENT_LOG");
    if (configured && *configured)
        snprintf(g_path, sizeof(g_path), "%s", configured);

    g_level = parse_level(getenv("MMO_CLIENT_LOG_LEVEL"));

    const char* max_bytes = getenv("MMO_CLIENT_LOG_MAX_BYTES");
    if (max_bytes && *max_bytes) {
        long parsed = strtol(max_bytes, NULL, 10);
        if (parsed >= 4096) g_max_bytes = parsed;
    }

    const char* keep = getenv("MMO_CLIENT_LOG_KEEP");
    if (keep && *keep) {
        long parsed = strtol(keep, NULL, 10);
        if (parsed >= 1 && parsed <= 100) g_keep = (int)parsed;
    }

    if (g_level == CLIENT_LOG_OFF) { UNLOCK(); return; }

    ensure_directory(g_path);
    g_file = fopen(g_path, "a");

    /* Not being able to write a log is not a reason not to run. The player
     * gets a working game and the console still carries everything; only the
     * file is missing, and the line below says so where a developer will see
     * it. */
    if (!g_file) {
        fprintf(stderr, "[LOG] cannot open '%s': %s — continuing without a log file\n",
                g_path, strerror(errno));
        UNLOCK();
        return;
    }

    fseek(g_file, 0, SEEK_END);
    g_bytes = ftell(g_file);

    UNLOCK();

    CLOG_INFO("client log opened (level %s, rolling at %ld bytes, keeping %d)",
              LEVEL_TAG[g_level], g_max_bytes, g_keep);
}

void client_log_close(void) {
    LOCK();
    if (g_file) {
        fflush(g_file);
        fclose(g_file);
        g_file = NULL;
    }
    UNLOCK();
    LOCK_DESTROY();
}

ClientLogLevel client_log_level(void) { return g_level; }

void client_log(ClientLogLevel level, const char* file, int line,
                const char* fmt, ...) {
    if (level < g_level || level >= CLIENT_LOG_OFF) return;

    /* Formatted before the lock so a slow format does not hold other threads,
     * and into a fixed buffer so logging never allocates. */
    char message[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(message, sizeof(message), fmt, ap);
    va_end(ap);

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm_buf;
#ifdef _WIN32
    localtime_s(&tm_buf, &ts.tv_sec);
#else
    localtime_r(&ts.tv_sec, &tm_buf);
#endif

    /* Just the basename: a full build path is noise in every line, and the
     * player sending this file has no use for a directory on someone's
     * machine. */
    const char* base = file ? strrchr(file, '/') : NULL;
#ifdef _WIN32
    const char* back = file ? strrchr(file, '\\') : NULL;
    if (back && (!base || back > base)) base = back;
#endif
    base = base ? base + 1 : (file ? file : "?");

    char line_buf[1200];
    int n = snprintf(line_buf, sizeof(line_buf),
                     "[%02d:%02d:%02d.%03ld] [%s] %s:%d: %s\n",
                     tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec,
                     ts.tv_nsec / 1000000,
                     LEVEL_TAG[level], base, line, message);
    if (n <= 0) return;
    if (n >= (int)sizeof(line_buf)) n = (int)sizeof(line_buf) - 1;

    LOCK();

    if (g_file) {
        if (g_bytes + n > g_max_bytes) rotate_locked();
        if (g_file) {
            fwrite(line_buf, 1, (size_t)n, g_file);
            g_bytes += n;
            /* Flushed per line on purpose. The whole reason this file exists
             * is to survive a crash or a kill, and a buffered last second is
             * exactly the second worth having. */
            fflush(g_file);
        }
    }

    /* Warnings and errors also go to the console, for a developer running from
     * a terminal. Trace and debug do not: those fire per packet. */
    if (level >= CLIENT_LOG_WARN) fputs(line_buf, stderr);

    UNLOCK();
}
