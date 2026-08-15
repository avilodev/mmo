#ifndef ROUTES_H
#define ROUTES_H

#include "types.h"

#include "packet_handler.h"

#include <stdio.h>
#include <sys/types.h>
#include <sys/socket.h>

int process_packet(int client_fd, uint32_t character_id, ssize_t bytes, uint8_t* buffer);

#endif