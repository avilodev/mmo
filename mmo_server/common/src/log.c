/**
 * @file
 * Emit synchronized server logs and enforce per-call-site rate windows.
 */

#include "log.h"

#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>   // strcasecmp
#include <stdatomic.h>
#include <sys/random.h>
#include <sys/time.h>
#include <time.h>

#define LOG_LINE_MAX 2048

static LogLevel        g_level       = LOG_LEVEL_INFO;
static pthread_mutex_t g_out_lock    = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_rate_lock   = PTHREAD_MUTEX_INITIALIZER;
static int             g_show_source = 1;   // include file:line

/* The rolling file sink.
 *
 * Logging went to stdout and nowhere else, with no rotation and no file, and
 * scripts/start_servers.sh did not redirect it either -- so a server's history
 * lived only in whatever terminal happened to have started it. Guarded by the
 * same output mutex as the console, so a line lands whole in both.
 *
 * Nothing in this file may use the LOG_* macros: they call back into log_emit
 * below, and a failure inside the sink would recurse. Diagnostics here go
 * straight to stderr.
 */
static FILE*  g_file        = NULL;
static char   g_file_path[512];
static long   g_file_bytes  = 0;
static long   g_file_max    = 32L * 1024 * 1024;
static int    g_file_keep   = 5;
static int    g_to_console  = 1;

/** Read a long from the environment, or return a default. */
static long env_long(const char* name, long fallback, long low, long high) {
    const char* raw = getenv(name);
    if (!raw || !*raw) return fallback;

    char* end = NULL;
    long parsed = strtol(raw, &end, 10);
    if (end == raw || *end || parsed < low || parsed > high) return fallback;
    return parsed;
}

/** Roll <path>.N-1 to <path>.N and the live file to <path>.1.
 *
 * Caller holds g_out_lock.
 */
static void rotate_locked(void) {
    if (!g_file) return;

    fclose(g_file);
    g_file = NULL;

    char from[600], to[600];

    /* Drop the oldest, then shift each survivor up one. */
    snprintf(to, sizeof(to), "%s.%d", g_file_path, g_file_keep);
    remove(to);
    for (int i = g_file_keep - 1; i >= 1; i--) {
        snprintf(from, sizeof(from), "%s.%d", g_file_path, i);
        snprintf(to,   sizeof(to),   "%s.%d", g_file_path, i + 1);
        rename(from, to);
    }
    snprintf(to, sizeof(to), "%s.1", g_file_path);
    rename(g_file_path, to);

    g_file = fopen(g_file_path, "a");
    g_file_bytes = 0;
    if (!g_file) {
        /* Console still works; say so once rather than silently losing the file. */
        fprintf(stderr, "log: cannot reopen '%s' after rotation: %s\n",
                g_file_path, strerror(errno));
    }
}

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

/** Initialize the runtime log level and source-location setting from the environment. */
void log_init(void) {
    g_level = parse_level(getenv("MMO_LOG_LEVEL"));

    const char* src = getenv("MMO_LOG_SOURCE");
    if (src && (strcmp(src, "0") == 0 || strcasecmp(src, "off") == 0))
        g_show_source = 0;

    const char* console = getenv("MMO_LOG_CONSOLE");
    if (console && (strcmp(console, "0") == 0 || strcasecmp(console, "off") == 0))
        g_to_console = 0;

    g_file_max  = env_long("MMO_LOG_MAX_BYTES", g_file_max, 4096, 1L << 40);
    g_file_keep = (int)env_long("MMO_LOG_KEEP", g_file_keep, 1, 1000);

    const char* path = getenv("MMO_LOG_FILE");
    if (!path || !*path) return;

    pthread_mutex_lock(&g_out_lock);
    if (!g_file) {
        snprintf(g_file_path, sizeof(g_file_path), "%s", path);
        g_file = fopen(g_file_path, "a");
        if (g_file) {
            setvbuf(g_file, NULL, _IOLBF, 0);   /* a crash must not eat the last lines */
            fseek(g_file, 0, SEEK_END);
            g_file_bytes = ftell(g_file);
            if (g_file_bytes < 0) g_file_bytes = 0;
        } else {
            fprintf(stderr, "log: cannot open '%s': %s — logging to the console only\n",
                    g_file_path, strerror(errno));
            /* Never leave both sinks off: a silent server is worse than a noisy one. */
            g_to_console = 1;
        }
    }
    pthread_mutex_unlock(&g_out_lock);
}

/** Close the log file, if one is open. */
void log_close(void) {
    pthread_mutex_lock(&g_out_lock);
    if (g_file) {
        fclose(g_file);
        g_file = NULL;
    }
    pthread_mutex_unlock(&g_out_lock);
}

/** Set the maximum enabled logging level. */
void log_set_level(LogLevel level) { g_level = level; }

/** Return the maximum enabled logging level. */
LogLevel log_get_level(void) { return g_level; }

/**
 * Test whether a logging level is enabled.
 *
 * @return      Nonzero when the level should be emitted, otherwise zero.
 */
int log_level_enabled(LogLevel level) { return (int)level <= (int)g_level; }

// Strip directories so "world_server/src/packet_handler.c" logs as
// "packet_handler.c" and lines stay readable.
static const char* basename_of(const char* path) {
    const char* slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

/**
 * Format and emit one timestamped log record without interleaving threads.
 *
 * This function serializes writes through the process-wide output mutex.
 */
/* --- Correlation --------------------------------------------------------- */

/** The trace id this thread is carrying. Empty when it is carrying none.
 *
 * Thread-local rather than a field on the connection: every function that logs
 * would otherwise need the id passed to it, which is most of them. A thread
 * adopts an id when it picks up a player's work and clears it when it puts it
 * down; a stale id on a pooled thread attributes one player's lines to
 * another, which is worse than having no id at all.
 */
static _Thread_local char t_trace[TRACE_ID_LEN] = {0};

void log_set_trace(const char* trace_id) {
    if (!trace_id || !*trace_id) { t_trace[0] = '\0'; return; }
    snprintf(t_trace, sizeof(t_trace), "%s", trace_id);
}

void log_clear_trace(void) { t_trace[0] = '\0'; }

const char* log_get_trace(void) { return t_trace; }

void log_new_trace(char* out) {
    if (!out) return;

    static const char hex[] = "0123456789abcdef";
    unsigned char bytes[TRACE_ID_LEN / 2];

    size_t filled = 0;
    while (filled < sizeof(bytes)) {
        ssize_t got = getrandom(bytes + filled, sizeof(bytes) - filled, 0);
        if (got > 0) { filled += (size_t)got; continue; }
        if (got < 0 && errno == EINTR) continue;
        break;
    }

    if (filled < sizeof(bytes)) {
        /* Entropy is unavailable. A duplicate trace id makes a log confusing,
         * not insecure -- nothing is authorised by one -- so this falls back
         * rather than failing and leaving the whole login untraceable. */
        static _Atomic unsigned long counter;
        unsigned long mix = (unsigned long)time(NULL) ^
                            (atomic_fetch_add(&counter, 1UL) * 0x9E3779B97F4A7C15UL);
        for (size_t i = filled; i < sizeof(bytes); i++) {
            bytes[i] = (unsigned char)(mix >> ((i % 8) * 8));
        }
    }

    for (size_t i = 0; i < sizeof(bytes); i++) {
        out[i * 2]     = hex[bytes[i] >> 4];
        out[i * 2 + 1] = hex[bytes[i] & 0x0F];
    }
    out[sizeof(bytes) * 2] = '\0';
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

    /* Formatted once, so the console and the file cannot disagree, and so the
     * file's size accounting matches what was actually written. */
    /* The trace id, when this thread is carrying one. Written as "[id] " so a
     * line without one looks exactly as it always did -- every existing log
     * grep keeps working, and `grep <id>` across all three services' files
     * gathers one player's whole login. */
    char trace[TRACE_ID_LEN + 4] = {0};
    if (t_trace[0]) snprintf(trace, sizeof(trace), "[%s] ", t_trace);

    char line_buf[LOG_LINE_MAX + 128];
    int  line_len;
    if (g_show_source)
        line_len = snprintf(line_buf, sizeof(line_buf), "[%s] [%s] %s%s:%d: %s\n",
                            stamp, LEVEL_TAG[level], trace,
                            basename_of(file), line, msg);
    else
        line_len = snprintf(line_buf, sizeof(line_buf), "[%s] [%s] %s%s\n",
                            stamp, LEVEL_TAG[level], trace, msg);
    if (line_len < 0) return;
    if ((size_t)line_len >= sizeof(line_buf)) line_len = (int)sizeof(line_buf) - 1;

    pthread_mutex_lock(&g_out_lock);

    if (g_to_console) fputs(line_buf, out);

    if (g_file) {
        if (g_file_bytes + line_len > g_file_max) rotate_locked();
        if (g_file) {
            fputs(line_buf, g_file);
            g_file_bytes += line_len;
        }
    }

    pthread_mutex_unlock(&g_out_lock);
}

/**
 * Consume one event from a shared rate window when capacity remains.
 *
 * This function serializes rate-state updates through the process-wide rate mutex.
 *
 * @param out_suppressed  Receives the prior window's suppressed count when a new window begins; may be NULL.
 * @return                Nonzero when the caller may emit, otherwise zero.
 */
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
