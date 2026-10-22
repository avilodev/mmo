#ifndef NET_SEND_H
#define NET_SEND_H

// Send framed packets on world-server connections.

#include "types.h"

#include <sys/types.h>

// set payload size, suppress SIGPIPE, and return bytes or -1
ssize_t server_send(int fd, void* buf, size_t len);

// complete writes for unregistered pre-authentication or realm sockets
ssize_t server_send_direct(int fd, void* buf, size_t len);

#endif /* NET_SEND_H */
