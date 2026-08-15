#define _POSIX_C_SOURCE 200809L

#include "utils.h"
#include "connection_io.h"
#include <errno.h>
#include <string.h>
#include <arpa/inet.h>
#include <sys/socket.h>

double get_time_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1000000000.0;
}

// Helper function for current time in seconds
double get_current_time(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return ts.tv_sec + ts.tv_nsec / 1000000000.0;
}

ssize_t server_send(int fd, void* buf, size_t len) {
    if (fd < 0 || !buf || len == 0) return -1;
    // Auto-set payload_size from the actual send length so the client
    // can use header.payload_size for framing without any special cases.
    if (len >= sizeof(PacketHeader)) {
        PacketHeader* hdr = (PacketHeader*)buf;
        hdr->payload_size = htons((uint16_t)(len - sizeof(PacketHeader)));
    }
    ssize_t accepted = connection_io_send(fd, buf, len);
    return accepted == -2 ? -1 : accepted;
}

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
            fprintf(stderr, "[NET] send() failed on fd %d: %s\n", fd, strerror(errno));
        }
        return -1;
    }
    return (ssize_t)offset;
}
