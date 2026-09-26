#ifndef PEER_ADDR_H
#define PEER_ADDR_H

/** @file Resolve a connected socket's peer address.
 *
 * Three services need the peer address for three different reasons — rate
 * limiting, session binding, and the realm allowlist — and each had grown its
 * own copy. One implementation means one place where IPv6 and getpeername
 * failure are handled.
 */

#include <stddef.h>
#include <sys/socket.h>

/** Fits an IPv6 literal plus its terminator (INET6_ADDRSTRLEN is 46). */
#define PEER_ADDR_MAXLEN 46

/**
 * Write a socket peer's address as text.
 *
 * Failure is reported as an empty string rather than an error code: every
 * caller treats "unknown peer" the same way it treats a failed lookup.
 *
 * @param out       Buffer receiving the address; set to "" when unavailable.
 * @param out_size  Size of that buffer.
 */
void peer_addr_text(int fd, char* out, size_t out_size);

/**
 * Read a socket peer's address into a sockaddr_storage.
 *
 * @return 1 on success, or 0 when the descriptor has no peer.
 */
int peer_addr_of(int fd, struct sockaddr_storage* out);

#endif // PEER_ADDR_H
