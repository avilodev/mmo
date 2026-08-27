#ifndef CLIENT_LOG_H
#define CLIENT_LOG_H

/** @file A rolling log file, so a player's disconnect leaves something behind.
 *
 * Everything the client had to say went to stdout, and a released client has
 * no console: a player whose session dropped could report only that it
 * dropped. NET_LOG was compiled out unless someone built with NET_DEBUG, so
 * the packet-level detail that would explain a disconnect did not exist in any
 * shipped build, and NET_WARN went to a stream nobody could see.
 *
 * This writes the same lines to a file beside the game, timestamped, and rolls
 * it when it gets large. It is the client counterpart of the server's
 * common/src/log.c and follows the same shape deliberately -- a player
 * reporting a problem and a server operator investigating it should be reading
 * two halves of one story.
 *
 * Writes are serialised, so any thread may log. Nothing here allocates.
 *
 * Every diagnostic in Game/src goes through this. It did not always: the
 * network layer logged properly through NET_LOG/NET_WARN, and everything else
 * -- the state machine, the game loop, asset loading, the player -- printed to
 * a console a released client does not have, which is to say a player's crash
 * report contained a fraction of what happened to them.
 * Game/tests/check_client_logging.sh fails the build if a printf comes back;
 * client_log.c itself is the one exemption, because a log file that would not
 * open cannot report that through itself.
 *
 * Levels, as used across the tree:
 *
 *   ERROR  something the player noticed, or an asset that would not load
 *   WARN   a refusal or a malformed file the game worked around
 *   INFO   the shape of the session: startup, state changes, connections
 *   DEBUG  per-action chatter -- clicks, casts, drags, texture loads
 *   TRACE  per-packet detail
 *
 * INFO is the default, so a shipped client's log is the session's story with
 * the per-frame noise left out. Warnings and errors also reach the console for
 * anyone running from a terminal.
 */

#include <stdarg.h>

/** Severity, low to high. Lines below the active level are not written. */
typedef enum {
    CLIENT_LOG_TRACE = 0,   /**< Per-packet detail. Off unless asked for. */
    CLIENT_LOG_DEBUG,
    CLIENT_LOG_INFO,
    CLIENT_LOG_WARN,
    CLIENT_LOG_ERROR,
    CLIENT_LOG_OFF,         /**< Level only; never pass to client_log(). */
} ClientLogLevel;

/**
 * Open the log file and start writing.
 *
 * Reads two environment variables, both optional:
 *   MMO_CLIENT_LOG        path to write to; default "Game/logs/client.log"
 *   MMO_CLIENT_LOG_LEVEL  trace|debug|info|warn|error|off; default info
 *
 * Failing to open the file is not fatal and not reported to the player: a
 * client that will not start because it could not write a log is worse than
 * one with no log. Console output continues either way.
 */
void client_log_init(void);

/** Flush and close. Safe when init failed or was never called. */
void client_log_close(void);

/** Write one line. Adds the timestamp, the level and the newline. */
void client_log(ClientLogLevel level, const char* file, int line,
                const char* fmt, ...)
#ifdef __GNUC__
    __attribute__((format(printf, 4, 5)))
#endif
    ;

/** The active level, for skipping work a suppressed line would have cost. */
ClientLogLevel client_log_level(void);

#define CLIENT_LOG_AT(level, ...)                                           \
    do {                                                                    \
        if ((level) >= client_log_level())                                  \
            client_log((level), __FILE__, __LINE__, __VA_ARGS__);           \
    } while (0)

#define CLOG_TRACE(...) CLIENT_LOG_AT(CLIENT_LOG_TRACE, __VA_ARGS__)
#define CLOG_DEBUG(...) CLIENT_LOG_AT(CLIENT_LOG_DEBUG, __VA_ARGS__)
#define CLOG_INFO(...)  CLIENT_LOG_AT(CLIENT_LOG_INFO,  __VA_ARGS__)
#define CLOG_WARN(...)  CLIENT_LOG_AT(CLIENT_LOG_WARN,  __VA_ARGS__)
#define CLOG_ERROR(...) CLIENT_LOG_AT(CLIENT_LOG_ERROR, __VA_ARGS__)

#endif // CLIENT_LOG_H
