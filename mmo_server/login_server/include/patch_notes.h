#ifndef PATCH_NOTES_H
#define PATCH_NOTES_H

#include "types.h"

#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <arpa/inet.h>

#define PATCH_NOTES_PATH "/home/avilo/mmo_server/server_files/patch_notes.txt" 

void handle_patch_notes_request(int client_fd, PacketHeader* packet, ssize_t bytes);

#endif 