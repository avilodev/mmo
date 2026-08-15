#ifndef LOG_H
#define LOG_H

// ============================================================================
// log.h — Leveled, thread-safe, rate-limited logging for all server processes
//
// Replaces bare printf() so that per-packet and per-tick code paths cannot
// flood stdout. A hostile client that spams a rejected opcode should cost us
// a bounded number of log lines per minute, not one line per packet.
//
// Level is chosen at runtime from the MMO_LOG_LEVEL environment variable:
//
//     MMO_LOG_LEVEL=error|warn|info|debug|trace     (default: info)
//
// Call log_init() once at startup, before any other thread is created.
//
// Usage:
//     LOG_INFO("world %s listening on port %d", name, port);
//     LOG_ERROR("failed to bind: %s", strerror(errno));
//
// For anything reachable from a packet handler or a tick loop, use the
// rate-limited forms so the volume stays bounded no matter what a client does:
//
//     LOG_WARN_RL(5, 60, "move rejected for char %u", character_id);
//
// which emits at most 5 lines per 60 seconds *per call site*, then reports
// how many were suppressed when the window rolls over.
// ============================================================================

#include <stdint.h>
#include <time.h>

typedef enum {
    LOG_LEVEL_ERROR = 0,
    LOG_LEVEL_WARN  = 1,
    LOG_LEVEL_INFO  = 2,
    LOG_LEVEL_DEBUG = 3,
    LOG_LEVEL_TRACE = 4
} LogLevel;

// Read MMO_LOG_LEVEL and prepare the output lock. Safe to call more than once.
void log_init(void);

void     log_set_level(LogLevel level);
LogLevel log_get_level(void);

// Cheap inline-able gate so disabled log statements cost one comparison and
// never evaluate their arguments.
int log_level_enabled(LogLevel level);

// Emit one formatted line. Prefer the macros below over calling this directly.
void log_emit(LogLevel level, const char* file, int line, const char* fmt, ...)
    __attribute__((format(printf, 4, 5)));

// ---------------------------------------------------------------------------
// Per-call-site rate limiting
// ---------------------------------------------------------------------------

typedef struct {
    time_t   window_start;   // start of the current counting window
    uint32_t emitted;        // lines emitted in this window
    uint32_t suppressed;     // lines dropped in this window
} LogRateState;

// Returns 1 if the caller may emit now, 0 if it is being suppressed.
// When it returns 1 and lines were dropped since the last emit, the count is
// written to *out_suppressed so it can be reported alongside the message.
int log_rate_allow(LogRateState* st, uint32_t max_per_window,
                   uint32_t window_secs, uint32_t* out_suppressed);

// ---------------------------------------------------------------------------
// Macros
// ---------------------------------------------------------------------------

#define LOG_AT(level, ...)                                                     \
    do {                                                                       \
        if (log_level_enabled(level))                                          \
            log_emit((level), __FILE__, __LINE__, __VA_ARGS__);                \
    } while (0)

#define LOG_ERROR(...) LOG_AT(LOG_LEVEL_ERROR, __VA_ARGS__)
#define LOG_WARN(...)  LOG_AT(LOG_LEVEL_WARN,  __VA_ARGS__)
#define LOG_INFO(...)  LOG_AT(LOG_LEVEL_INFO,  __VA_ARGS__)
#define LOG_DEBUG(...) LOG_AT(LOG_LEVEL_DEBUG, __VA_ARGS__)
#define LOG_TRACE(...) LOG_AT(LOG_LEVEL_TRACE, __VA_ARGS__)

// Rate-limited variants. Each expansion owns a private static counter, so two
// different call sites never share a budget.
#define LOG_AT_RL(level, max, window, fmt, ...)                                \
    do {                                                                       \
        if (log_level_enabled(level)) {                                        \
            static LogRateState _log_rl_state;                                 \
            uint32_t _log_rl_dropped = 0;                                      \
            if (log_rate_allow(&_log_rl_state, (max), (window),                \
                               &_log_rl_dropped)) {                            \
                if (_log_rl_dropped)                                           \
                    log_emit((level), __FILE__, __LINE__,                      \
                             fmt " [+%u suppressed]", ##__VA_ARGS__,           \
                             _log_rl_dropped);                                 \
                else                                                           \
                    log_emit((level), __FILE__, __LINE__, fmt,                 \
                             ##__VA_ARGS__);                                   \
            }                                                                  \
        }                                                                      \
    } while (0)

#define LOG_ERROR_RL(max, window, ...) LOG_AT_RL(LOG_LEVEL_ERROR, max, window, __VA_ARGS__)
#define LOG_WARN_RL(max, window, ...)  LOG_AT_RL(LOG_LEVEL_WARN,  max, window, __VA_ARGS__)
#define LOG_INFO_RL(max, window, ...)  LOG_AT_RL(LOG_LEVEL_INFO,  max, window, __VA_ARGS__)
#define LOG_DEBUG_RL(max, window, ...) LOG_AT_RL(LOG_LEVEL_DEBUG, max, window, __VA_ARGS__)

#endif // LOG_H
