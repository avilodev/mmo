#ifndef WORLD_CONNECT_H
#define WORLD_CONNECT_H

/** @file Expose realm connections and configured world-server discovery. */

#include "types.h"
#include "realm_world_auth.h"
#include "session.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <arpa/inet.h>
#include <sys/socket.h> 
#include <sys/types.h>
#include <unistd.h>
#include <ctype.h>

#include <poll.h>
#include <signal.h>

// return a connected descriptor or -1, suppressing diagnostics when silent
int connect_to_world_server(const char* host, int port, const char* server_key, int silent);
// return the number of server records loaded up to max_servers
int load_world_servers_from_file(const char* filepath, WorldServer* servers, int max_servers);

#endif
