#ifndef TYPES_H
#define TYPES_H

#include "headers.h"

#define USERS_DB "../database/databases/users_data/users.db"

#define MIN_HEADER_SIZE 7
#define AUTH_REGISTER_SIZE 215

#define LOGIN_SERVER_PORT 7776
#define REALM_SERVER_PORT 7777
#define WORLD_SERVER_PORT 7778

#define MAX_PLAYERS 1000
#define MAX_PENDING_CONNECTIONS 10
#define TICK_RATE 60.0f
#define TIMEOUT_SECONDS 30.0f
#define SESSION_EXPIRY_SECONDS 300
#define MAX_PACKET_SIZE 8192

#endif
