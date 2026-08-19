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

// set payload size, suppress SIGPIPE, and return bytes or -1
ssize_t server_send(int fd, void* buf, size_t len);

// complete writes for unregistered pre-authentication or realm sockets
ssize_t server_send_direct(int fd, void* buf, size_t len);

#endif
