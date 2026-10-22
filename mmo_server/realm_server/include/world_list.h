#ifndef WORLD_LIST_H
#define WORLD_LIST_H

#include "types.h"

#include "config.h"
#include "character_connect.h"

#include <stdio.h>
#include <string.h>
#include <arpa/inet.h>
#include <sys/types.h>

void world_send_list(int client_fd, uint32_t account_id);           
void world_enter(int client_fd, uint32_t account_id, uint8_t* buffer, ssize_t bytes);

#endif
