#ifndef ROUTES_H
#define ROUTES_H

#include "types.h"
#include "auth.h"

#include <sys/types.h>

// Login server route
void route_packet(int client_fd, void* packet_data, ssize_t bytes);

// Realm server route  
int process_packet(int client_fd, uint32_t account_id, uint8_t* buffer, ssize_t bytes);

#endif