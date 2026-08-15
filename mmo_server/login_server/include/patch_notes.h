#ifndef PATCH_NOTES_H
#define PATCH_NOTES_H

#include "types.h"

#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <arpa/inet.h>

// Path relative to the server's working directory (#11)
// Run the login server from its base directory, e.g.:
//   cd /home/user/mmo_server/login_server && ./login_server
#define PATCH_NOTES_PATH "./server_files/patch_notes.txt"
#define PATCH_NOTES_MAX_SIZE 4000

void patch_notes_init(void);
void handle_patch_notes_request(int client_fd, PacketHeader* packet, ssize_t bytes);

#endif 