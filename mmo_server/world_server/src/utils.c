#define _POSIX_C_SOURCE 200809L

/**
 * @file
 * Provide world-server clocks and framed socket send helpers.
 */

#include "utils.h"
#include "log.h"
#include "connection_io.h"
#include <errno.h>
#include <string.h>
#include <arpa/inet.h>
#include <sys/socket.h>

/**
 * Read the monotonic clock as fractional seconds.
 *
 * @return      Monotonic time in seconds.
 */
double get_time_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1000000000.0;
}

/**
 * Read the realtime clock as fractional seconds.
 *
 * @return      Realtime in seconds since the Unix epoch.
 */
double get_current_time(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return ts.tv_sec + ts.tv_nsec / 1000000000.0;
}

/**
 * Queue a framed packet for a registered connection.
 *
 * The function writes the network-order payload size into buf before queuing it.
 *
 * @param fd  Registered connection descriptor.
 * @param buf  Mutable packet beginning with PacketHeader.
 * @param len  Total packet length in bytes.
 * @return      Accepted byte count, or -1 on invalid input or queue failure.
 */
ssize_t server_send(int fd, void* buf, size_t len) {
    if (fd < 0 || !buf || len == 0) return -1;
    // derive framing length from the supplied packet
    if (len >= sizeof(PacketHeader)) {
        PacketHeader* hdr = (PacketHeader*)buf;
        hdr->payload_size = htons((uint16_t)(len - sizeof(PacketHeader)));
    }
    ssize_t accepted = connection_io_send(fd, buf, len);
    return accepted == -2 ? -1 : accepted;
}

/**
 * Send an entire framed packet directly on a socket.
 *
 * The function blocks until all bytes are sent or an error occurs and writes the payload size into buf.
 *
 * @param fd  Socket descriptor not managed by the connection queue.
 * @param buf  Mutable packet beginning with PacketHeader.
 * @param len  Total packet length in bytes.
 * @return      Total bytes sent, or -1 on invalid input or send failure.
 */
ssize_t server_send_direct(int fd, void* buf, size_t len) {
    if (fd < 0 || !buf || len == 0) return -1;
    if (len >= sizeof(PacketHeader)) {
        PacketHeader* hdr = (PacketHeader*)buf;
        hdr->payload_size = htons((uint16_t)(len - sizeof(PacketHeader)));
    }
    size_t offset = 0;
    while (offset < len) {
        ssize_t sent = send(fd, (uint8_t*)buf + offset, len - offset, MSG_NOSIGNAL);
        if (sent > 0) {
            offset += (size_t)sent;
            continue;
        }
        if (sent < 0 && errno == EINTR) continue;
        if (sent < 0 && errno != EPIPE && errno != ECONNRESET) {
            LOG_ERROR("[NET] send() failed on fd %d: %s", fd, strerror(errno));
        }
        return -1;
    }
    return (ssize_t)offset;
}
