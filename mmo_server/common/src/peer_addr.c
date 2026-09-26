/**
 * @file
 * Resolve a connected socket's peer address as text or as a sockaddr.
 */
#include "peer_addr.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <string.h>

/** Read the peer address of a connected descriptor. */
int peer_addr_of(int fd, struct sockaddr_storage* out) {
    if (!out) return 0;

    socklen_t len = sizeof(*out);
    memset(out, 0, sizeof(*out));
    return getpeername(fd, (struct sockaddr*)out, &len) == 0;
}

/** Render the peer address of a connected descriptor as text. */
void peer_addr_text(int fd, char* out, size_t out_size) {
    if (!out || out_size == 0) return;
    out[0] = '\0';

    struct sockaddr_storage addr;
    if (!peer_addr_of(fd, &addr)) return;

    if (addr.ss_family == AF_INET) {
        struct sockaddr_in* v4 = (struct sockaddr_in*)&addr;
        inet_ntop(AF_INET, &v4->sin_addr, out, (socklen_t)out_size);
    } else if (addr.ss_family == AF_INET6) {
        struct sockaddr_in6* v6 = (struct sockaddr_in6*)&addr;
        inet_ntop(AF_INET6, &v6->sin6_addr, out, (socklen_t)out_size);
    }
}
