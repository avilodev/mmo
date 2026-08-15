#ifndef UTILS_H
#define UTILS_H

#include "types.h"
#include "session.h"
#include "players_database.h"

#include <stdio.h>
#include <stdint.h>
#include <time.h>
#include <signal.h>

double get_time_seconds(void);
double get_current_time(void);

// Safe send wrapper — uses MSG_NOSIGNAL to prevent SIGPIPE.
// Automatically sets header.payload_size from the actual send length.
// Returns bytes sent on success, -1 on error.
ssize_t server_send(int fd, void* buf, size_t len);

// Complete a framed write on a socket that is not a registered player
// connection, such as pre-authentication and realm-server sockets.
ssize_t server_send_direct(int fd, void* buf, size_t len);

#endif
