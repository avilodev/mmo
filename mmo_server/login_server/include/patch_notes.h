#ifndef PATCH_NOTES_H
#define PATCH_NOTES_H

#include "types.h"

#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <arpa/inet.h>

/** Locate and bound patch-note text relative to the login server's working directory. */
#define PATCH_NOTES_PATH "./server_files/patch_notes.txt"
#define PATCH_NOTES_MAX_SIZE 4000

void patch_notes_init(void);
void handle_patch_notes_request(int client_fd, PacketHeader* packet, ssize_t bytes);

#endif 