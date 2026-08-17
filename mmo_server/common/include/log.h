#ifndef LOG_H
#define LOG_H

/** @file Provide leveled, thread-safe logging with per-call-site rate limits.
 * MMO_LOG_LEVEL selects error, warn, info, debug, or trace at startup.
 */

#include <stdint.h>
#include <time.h>

/** Order server log levels from least to most verbose. */
typedef enum {
    LOG_LEVEL_ERROR = 0,
    LOG_LEVEL_WARN  = 1,
    LOG_LEVEL_INFO  = 2,
    LOG_LEVEL_DEBUG = 3,
    LOG_LEVEL_TRACE = 4
} LogLevel;

// initialize idempotently before starting worker threads
void log_init(void);

void     log_set_level(LogLevel level);
LogLevel log_get_level(void);

// avoid evaluating arguments for disabled levels
int log_level_enabled(LogLevel level);

// preserve printf format checking for direct emission
void log_emit(LogLevel level, const char* file, int line, const char* fmt, ...)
    __attribute__((format(printf, 4, 5)));

/** Track one call site's current rate-limit window. */
typedef struct {
    time_t   window_start;   /**< Start time of the current window. */
    uint32_t emitted;        /**< Lines emitted during the current window. */
    uint32_t suppressed;     /**< Lines dropped during the current window. */
} LogRateState;

// return permission and report drops from the preceding window
int log_rate_allow(LogRateState* st, uint32_t max_per_window,
                   uint32_t window_secs, uint32_t* out_suppressed);

/** Emit at a level while avoiding evaluation when that level is disabled. */
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

/** Rate-limit each macro expansion through its private static counter. */
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
