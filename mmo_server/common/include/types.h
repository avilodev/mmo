#ifndef TYPES_H
#define TYPES_H

#include "headers.h"

/** Locate the legacy user database relative to a server process. */
#define USERS_DB "../database/databases/users_data/users.db"

/** Define fixed protocol framing sizes in bytes. */
#define MIN_HEADER_SIZE 7
#define AUTH_REGISTER_SIZE 215

/** Assign default ports to the login, realm, and world services. */
#define LOGIN_SERVER_PORT 7776
#define REALM_SERVER_PORT 7777
#define WORLD_SERVER_PORT 7778

/** Configure shared connection, tick, timeout, session, and packet limits. */
#define MAX_PLAYERS 1000
#define MAX_PENDING_CONNECTIONS 10
#define TICK_RATE 60.0f
#define TIMEOUT_SECONDS 30.0f
#define SESSION_EXPIRY_SECONDS 300
#define MAX_PACKET_SIZE 8192

#endif
