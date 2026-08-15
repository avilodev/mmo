#ifndef CONNECTION_IO_H
#define CONNECTION_IO_H

#include <stddef.h>
#include <sys/types.h>

void connection_io_init(void);
void connection_io_shutdown(void);
int connection_io_register(int fd);
void connection_io_unregister(int fd);

// Accepts a complete framed packet for delivery. Returns len when the packet
// was sent or queued, -1 on a socket/queue failure, and -2 for an unregistered fd.
ssize_t connection_io_send(int fd, const void* data, size_t len);
int connection_io_flush(int fd);
int connection_io_has_pending(int fd);
int connection_io_failed(int fd);

#endif
