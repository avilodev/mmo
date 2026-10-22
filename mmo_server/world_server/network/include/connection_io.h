#ifndef CONNECTION_IO_H
#define CONNECTION_IO_H

/** @file Expose registered connection queues and nonblocking packet delivery. */

#include <stddef.h>
#include <sys/types.h>

void connection_io_init(void);
void connection_io_shutdown(void);
int connection_io_register(int fd);
void connection_io_unregister(int fd);

// return len, -1 for I/O failure, or -2 when unregistered
ssize_t connection_io_send(int fd, const void* data, size_t len);
int connection_io_flush(int fd);
int connection_io_has_pending(int fd);
int connection_io_failed(int fd);

#endif
