#ifndef NET_CONNECT_H
#define NET_CONNECT_H

/**
 * @file
 * Drive realm and world connections without stopping the frame.
 *
 * Connecting used to be one blocking call: a blocking connect(), then a loop
 * that called network_update() and Sleep(10) until an acknowledgement arrived
 * or ten seconds passed. It ran on the render thread, from inside the frame
 * loop, and the frame loop retried it every five seconds — so with the realm
 * down the window stopped answering the operating system for the connect
 * timeout plus ten seconds of polling, over and over, and Windows drew it as
 * "not responding".
 *
 * The work is the same; what changes is who waits. begin_* starts a
 * non-blocking connect and returns immediately, and poll() advances the
 * handshake by whatever has actually happened since the last frame. A frame
 * costs one non-blocking check.
 */

#include <stdint.h>

/** Where an in-flight connection attempt has got to. */
typedef enum {
    NET_CONNECT_IDLE = 0,   /**< Nothing in flight. */
    NET_CONNECT_PENDING,    /**< A connect or handshake is still in progress. */
    NET_CONNECT_SUCCEEDED,  /**< The session is live. Reported once. */
    NET_CONNECT_FAILED      /**< The attempt is over. Reported once. */
} NetConnectPhase;

/** Start connecting to a realm server.
 *
 * @param session_key  32 binary session-key bytes.
 * @return 1 when the attempt started, or 0 when it could not be (no socket,
 *         unusable address). A 0 leaves no attempt in flight.
 */
int network_begin_realm_connect(const char* host, uint16_t port,
                                const char* session_key, uint32_t account_id);

/** Start connecting to a world server, replacing any current connection.
 *
 * @param game_ticket  The 64-byte world ticket the realm issued.
 * @return 1 when the attempt started, otherwise 0.
 */
int network_begin_world_connect(const char* host, uint16_t port,
                                const char* game_ticket, uint32_t character_id);

/** Advance the current attempt. Call once per frame; never blocks.
 *
 * SUCCEEDED and FAILED are each reported exactly once and then become IDLE, so
 * a caller polling every frame cannot miss the result or act on it twice.
 */
NetConnectPhase network_connect_poll(void);

/** Report whether the attempt in flight is the realm handshake.
 *
 * The one-shot contract above means only the caller that started an attempt may
 * poll it. There are three: the frame loop drives the realm, character select
 * drives a world entry, and the reconnect supervisor drives both halves of a
 * recovery. The frame loop cannot tell them apart from
 * network_connect_in_flight() alone, and polling a world handshake there
 * consumed the result character select was waiting for.
 */
int network_realm_connect_in_flight(void);

/** Describe the outcome of the last attempt, for the status line. */
const char* network_connect_message(void);

/** Abandon any attempt in flight and close its socket. */
void network_connect_abort(void);

/** Report whether an attempt is currently in flight. */
int network_connect_in_flight(void);

#endif // NET_CONNECT_H
