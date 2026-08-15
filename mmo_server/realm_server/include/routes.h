#ifndef ROUTES_H
#define ROUTES_H

#include "types.h"
#include "world_list.h"
#include "users_database.h"
#include "character_connect.h"

#include <stdio.h>
#include <arpa/inet.h>
#include <string.h>
#include <sys/types.h>
 
int process_packet(int client_fd, uint32_t account_id, uint8_t* buffer, ssize_t bytes);

#endif 