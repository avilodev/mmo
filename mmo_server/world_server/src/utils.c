#define _POSIX_C_SOURCE 200809L

#include "utils.h"
#include <errno.h>
#include <string.h>
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

ssize_t server_send(int fd, const void* buf, size_t len) {
    if (fd < 0 || !buf || len == 0) return -1;
    ssize_t sent = send(fd, buf, len, MSG_NOSIGNAL);
    if (sent < 0) {
        // EPIPE/ECONNRESET = client disconnected, not worth logging loudly
        if (errno != EPIPE && errno != ECONNRESET) {
            fprintf(stderr, "[NET] send() failed on fd %d: %s\n", fd, strerror(errno));
        }
    }
    return sent;
}