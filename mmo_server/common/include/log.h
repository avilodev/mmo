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

/** Initialize logging from the environment, idempotently, before any thread starts.
 *
 * Reads:
 *   MMO_LOG_LEVEL      error | warn | info | debug | trace   (default info)
 *   MMO_LOG_SOURCE     0/off to omit file:line
 *   MMO_LOG_FILE       path to a rolling log file; unset means console only
 *   MMO_LOG_MAX_BYTES  rotate the file past this size   (default 33554432)
 *   MMO_LOG_KEEP       rotated files to retain          (default 5)
 *   MMO_LOG_CONSOLE    0/off to write only to the file
 */
void log_init(void);

/** Close the log file, if one is open. Safe to call more than once. */
void log_close(void);

void     log_set_level(LogLevel level);
LogLevel log_get_level(void);

// avoid evaluating arguments for disabled levels
int log_level_enabled(LogLevel level);

// preserve printf format checking for direct emission
void log_emit(LogLevel level, const char* file, int line, const char* fmt, ...)
    __attribute__((format(printf, 4, 5)));

/* --- Correlation ---------------------------------------------------------
 *
 * A player's login crosses three processes -- login, realm, world -- and until
 * this existed nothing tied their log lines together. account_id appeared in
 * some lines and not others, character_id in different ones, and the only way
 * to follow one player through a failed login was to line up timestamps across
 * three files and hope no one else logged in that second.
 *
 * A trace id is minted once, by the login server, when a session is created.
 * It travels with the session in Redis and with the world ticket, so the realm
 * and the world adopt the same one. Every line a thread emits while handling
 * that player's work carries it.
 *
 * Thread-local, because these services handle one player per thread at a time
 * and a per-connection field would have to be threaded through every function
 * that logs. Set it when a thread picks up work for a known player, clear it
 * when it puts it down -- a stale id on a pooled thread is worse than none,
 * because it attributes one player's lines to another.
 */

/** Characters in a trace id, plus the terminator. */
#define TRACE_ID_LEN 17

/** Adopt a trace id for this thread. NULL or empty clears it. */
void log_set_trace(const char* trace_id);

/** Stop attributing this thread's lines to any player. */
void log_clear_trace(void);

/** The trace id this thread is carrying; "" when none. Never NULL. */
const char* log_get_trace(void);

/** Generate a new trace id into `out`, which must hold TRACE_ID_LEN bytes.
 *
 * Random rather than sequential: an id that reveals how many logins have
 * happened is a number worth not publishing, and these appear in logs that get
 * pasted into tickets. Falls back to a clock- and counter-derived value when
 * the system entropy source is unavailable -- a duplicate trace id is a
 * confusing log, not a security problem, so this must not fail closed.
 */
void log_new_trace(char* out);

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
