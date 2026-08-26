#ifndef REALM_WORLD_AUTH_H
#define REALM_WORLD_AUTH_H

/** @file Expose Redis-backed realm/world keys and single-use game tickets. */

#include <stddef.h>
#include <stdint.h>
#include <hiredis/hiredis.h>


/** Minimum seconds the previous server key stays valid after a rotation.
 *
 * Rotation is not instantaneous across a fleet, and a key that becomes invalid
 * the instant a new one is written turns a routine rotation into a window
 * where the realm cannot authenticate to any world.
 */
#define SERVER_KEY_OVERLAP_SECONDS 900

// return a caller-owned key that must be freed
char* get_server_auth_key_from_redis(const char* world_name);

/** Store a server key, keeping the one it replaces valid for an overlap window. */
int set_server_auth_key_in_redis(const char* server_name, const char* auth_key, int ttl_seconds);

/** Accept a key matching either the current or the previous value for a world. */
int validate_server_auth_key(const char* provided_key, const char* world_name);

/** Compare two NUL-terminated keys without leaking the matching prefix length.
 *
 * A byte-at-a-time strcmp returns as soon as it finds a difference, so how long
 * it took to answer describes how much of a guess was right.
 */
int auth_key_equal(const char* a, const char* b);

/* validate_game_ticket() is declared in session.h, which owns the ticket
 * lifecycle. A second declaration lived here and drifted the moment the
 * function grew its peer-address argument.
 *
 * get_account_from_ticket() is gone. It read Redis key "game_ticket:%s" while
 * the realm has always stored "ticket:%s", so it could only ever return zero;
 * nothing called it, and leaving a mismatched key namespace in a header is an
 * invitation to start. */

#endif
