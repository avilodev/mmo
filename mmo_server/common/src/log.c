// ============================================================================
// log.c — Implementation of the leveled, rate-limited logger.
//
// Design notes:
//   - One mutex guards the output stream so lines from different threads never
//     interleave mid-message. The formatted line is built in a stack buffer
//     first, so the lock is held only for the write itself.
//   - ERROR and WARN go to stderr; everything else to stdout. Both are line
//     buffered when attached to a terminal and block buffered when redirected,
//     which is what the supervisor scripts want.
//   - Rate-limit state lives at the call site (see LOG_AT_RL); this file only
//     provides the window arithmetic, guarded by its own small lock.
// ============================================================================

#include "log.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>   // strcasecmp
#include <sys/time.h>

#define LOG_LINE_MAX 2048

static LogLevel        g_level       = LOG_LEVEL_INFO;
static pthread_mutex_t g_out_lock    = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_rate_lock   = PTHREAD_MUTEX_INITIALIZER;
static int             g_show_source = 1;   // include file:line

static const char* const LEVEL_TAG[] = {
    "ERROR", "WARN ", "INFO ", "DEBUG", "TRACE"
};

static LogLevel parse_level(const char* s) {
    if (!s) return LOG_LEVEL_INFO;
    if (strcasecmp(s, "error") == 0) return LOG_LEVEL_ERROR;
    if (strcasecmp(s, "warn")  == 0) return LOG_LEVEL_WARN;
    if (strcasecmp(s, "info")  == 0) return LOG_LEVEL_INFO;
    if (strcasecmp(s, "debug") == 0) return LOG_LEVEL_DEBUG;
    if (strcasecmp(s, "trace") == 0) return LOG_LEVEL_TRACE;
    return LOG_LEVEL_INFO;
}

void log_init(void) {
    g_level = parse_level(getenv("MMO_LOG_LEVEL"));

    const char* src = getenv("MMO_LOG_SOURCE");
    if (src && (strcmp(src, "0") == 0 || strcasecmp(src, "off") == 0))
        g_show_source = 0;
}

void log_set_level(LogLevel level) { g_level = level; }

LogLevel log_get_level(void) { return g_level; }

int log_level_enabled(LogLevel level) { return (int)level <= (int)g_level; }

// Strip directories so "world_server/src/packet_handler.c" logs as
// "packet_handler.c" and lines stay readable.
static const char* basename_of(const char* path) {
    const char* slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

void log_emit(LogLevel level, const char* file, int line, const char* fmt, ...) {
    if (!log_level_enabled(level)) return;
    if ((int)level < 0 || (int)level > LOG_LEVEL_TRACE) level = LOG_LEVEL_INFO;

    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm tm_buf;
    localtime_r(&tv.tv_sec, &tm_buf);

    char stamp[32];
    snprintf(stamp, sizeof(stamp), "%02d:%02d:%02d.%03d",
             tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec,
             (int)(tv.tv_usec / 1000));

    char msg[LOG_LINE_MAX];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    FILE* out = (level <= LOG_LEVEL_WARN) ? stderr : stdout;

    pthread_mutex_lock(&g_out_lock);
    if (g_show_source)
        fprintf(out, "[%s] [%s] %s:%d: %s\n",
                stamp, LEVEL_TAG[level], basename_of(file), line, msg);
    else
        fprintf(out, "[%s] [%s] %s\n", stamp, LEVEL_TAG[level], msg);
    pthread_mutex_unlock(&g_out_lock);
}

int log_rate_allow(LogRateState* st, uint32_t max_per_window,
                   uint32_t window_secs, uint32_t* out_suppressed) {
    if (!st) return 1;
    if (max_per_window == 0) max_per_window = 1;
    if (window_secs == 0) window_secs = 1;

    time_t now = time(NULL);
    int allowed = 0;

    pthread_mutex_lock(&g_rate_lock);

    // First use, or the window has rolled over: start a fresh budget and hand
    // back however many lines were dropped during the window that just ended.
    if (st->window_start == 0 ||
        (now - st->window_start) >= (time_t)window_secs) {
        if (out_suppressed) *out_suppressed = st->suppressed;
        st->window_start = now;
        st->emitted      = 1;
        st->suppressed   = 0;
        allowed          = 1;
    } else if (st->emitted < max_per_window) {
        if (out_suppressed) *out_suppressed = 0;
        st->emitted++;
        allowed = 1;
    } else {
        st->suppressed++;
        allowed = 0;
    }

    pthread_mutex_unlock(&g_rate_lock);
    return allowed;
}
