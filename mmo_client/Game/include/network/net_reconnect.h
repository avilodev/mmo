#ifndef NET_RECONNECT_H
#define NET_RECONNECT_H

/**
 * @file
 * Recover a dropped connection without ending the session.
 *
 * There was no recovery at all. A world connection that dropped — a server
 * restart, a router dropping an idle NAT entry, thirty seconds of bad wifi —
 * ended the session, and the player's only option was to close the client and
 * start again from the launcher. The realm case was worse than nothing: the
 * frame loop retried a blocking connect every five seconds, so a realm that
 * was down froze the window rather than reconnecting to it.
 *
 * Recovering an in-world session is not one connect. The world ticket is
 * single-use and lives sixty seconds, so it is long gone by the time the drop
 * is noticed; a new one has to be minted, which means going back to the realm.
 * The sequence is: reconnect to the realm, ask it to enter the same world as
 * the same character, take the fresh ticket to the world server. This drives
 * that, one step per frame, and gives up gracefully rather than hammering.
 */

#include <stdint.h>

/** Where the supervisor is in a recovery. */
typedef enum {
    RECONNECT_IDLE = 0,       /**< Connected, or nothing to recover. */
    RECONNECT_WAITING,        /**< Backing off before the next attempt. */
    RECONNECT_REALM,          /**< Reconnecting to the realm. */
    RECONNECT_TICKET,         /**< Asking the realm for a fresh world ticket. */
    RECONNECT_WORLD,          /**< Redeeming that ticket at the world server. */
    RECONNECT_GIVEN_UP        /**< Out of attempts; the player must act. */
} ReconnectPhase;

/** Remember what a live world session was, so it can be rebuilt.
 *
 * Called once the world handshake succeeds. Nothing else knows all three
 * facts at the same time: the realm knows the address, the character screen
 * knows the character, and only the moment of entry knows they went together.
 */
void net_reconnect_remember_world(const char* realm_ip, uint16_t realm_port,
                                  uint32_t account_id, const char* session_key_hex,
                                  uint32_t world_id, uint32_t character_id);

/** Forget the remembered session. Called on a deliberate logout. */
void net_reconnect_forget(void);

/** Report that the connection has dropped and recovery should begin.
 *
 * @param now  Seconds on the same monotonic clock net_reconnect_update() is
 *             given. Taken from the caller rather than read here, so the
 *             module has exactly one notion of time and a test can supply it.
 */
void net_reconnect_begin(double now);

/** Advance recovery by one frame's worth. Never blocks.
 *
 * @param now  Seconds on any monotonic clock, as the frame loop measures it.
 * @return     The phase after this step.
 */
ReconnectPhase net_reconnect_update(double now);

/** The current phase, without advancing it. */
ReconnectPhase net_reconnect_phase(void);

/** A short line describing the current state, for the status area. */
const char* net_reconnect_status(void);

/** Attempts made since the connection dropped. */
int net_reconnect_attempts(void);

/** Abandon recovery and reset the backoff, e.g. because the player logged out. */
void net_reconnect_cancel(void);

/** Seconds until the next attempt, or 0 when one is in flight or none is due. */
double net_reconnect_seconds_until_retry(double now);

#endif // NET_RECONNECT_H
