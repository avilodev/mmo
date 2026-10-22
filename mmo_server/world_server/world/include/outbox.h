#ifndef OUTBOX_H
#define OUTBOX_H

// Where game logic puts packets for players.
//
// Game logic hands a finished packet to the outbox and never sees a socket.
// The network layer registers the sender once at startup.

#include <stddef.h>
#include <sys/types.h>

// send one framed packet to a connection, return the bytes accepted or -1
typedef ssize_t (*outbox_sender)(int client_fd, void* packet, size_t length);

// register the network layer's sender; call before any player connects
void outbox_set_sender(outbox_sender sender);

// queue a packet for the connection, return the bytes accepted or -1
ssize_t outbox_send(int client_fd, void* packet, size_t length);

#endif /* OUTBOX_H */
