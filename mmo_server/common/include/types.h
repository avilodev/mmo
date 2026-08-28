#ifndef TYPES_H
#define TYPES_H

#include "headers.h"

/** Locate the legacy user database relative to a server process. */
#define USERS_DB "../database/databases/users_data/users.db"

/** Locate the friend graph, beside the account database it references.
 *
 * A separate file rather than more tables in users.db: the login server owns
 * that one and has no use for any of this, and the realm owns this one and
 * never writes accounts. Two owners, two files.
 */
#define SOCIAL_DB "../database/databases/users_data/social.db"

/** Define fixed protocol framing sizes in bytes. */
#define MIN_HEADER_SIZE 7
#define AUTH_REGISTER_SIZE 215

/** Assign default ports to the login, realm, and world services. */
#define LOGIN_SERVER_PORT 7776
#define REALM_SERVER_PORT 7777
#define WORLD_SERVER_PORT 7778

/** Configure shared connection and packet limits.
 *
 * Three constants used to live here and no longer do:
 *
 *   * TICK_RATE (60.0f) was read by nothing at all, and it disagreed with the
 *     20Hz gameplay tick the world server actually runs -- so the one thing it
 *     could have been used for was getting the tick rate wrong. The real rate
 *     is per-world configuration; see world_server/include/config.h.
 *   * TIMEOUT_SECONDS (30.0f) was likewise unreferenced. Idle timeouts belong
 *     to the reactor and are set in net_tuning.h.
 *   * SESSION_EXPIRY_SECONDS was defined here *and* in session.h with the same
 *     value. Sessions belong to session.h, which is where it stayed.
 *
 * Unused shared constants are not free: the next person to need a tick rate
 * finds one here, uses it, and is wrong.
 */
#define MAX_PLAYERS 1000
#define MAX_PENDING_CONNECTIONS 10
#define MAX_PACKET_SIZE 8192

#endif
