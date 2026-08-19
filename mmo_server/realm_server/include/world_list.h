#ifndef WORLD_LIST_H
#define WORLD_LIST_H

#include "types.h"
#include "session.h"

#include "config.h"
#include "character_connect.h"
#include "players_database.h"

#include <stdio.h>
#include <string.h>
#include <arpa/inet.h>
#include <sys/types.h>

/** Locate the realm's configured world-list file. */
#define WORLD_FILE_PATH "/home/avilo/mmo_server/realm_server/worlds/worlds.txt"

void world_send_list(int client_fd, uint32_t account_id);           
void world_enter(int client_fd, uint32_t account_id, uint8_t* buffer, ssize_t bytes);

#endif
