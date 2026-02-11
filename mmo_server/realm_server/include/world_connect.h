#ifndef WORLD_CONNECT_H
#define WORLD_CONNECT_H

#include "types.h"
#include "realm_world_auth.h"
#include "session.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <arpa/inet.h>
#include <sys/socket.h> 
#include <sys/types.h>
#include <ctype.h>

#include <poll.h>
#include <signal.h>

int connect_to_world_server(const char* host, int port, const char* server_key, int silent);
int load_world_servers_from_file(const char* filepath, WorldServer* servers, int max_servers);

#endif 