#ifndef NET_PING_WATCH_H
#define NET_PING_WATCH_H

// Notes the instant a ping's echo reaches this machine.

// The game reads its socket once a frame, so an echo that came in a tenth of a
// millisecond after the ping went out is not seen until the frame after, and a
// timer stopped there reads a whole frame (16 ms at 60 fps) whatever the link.
// This watches from a thread of its own. It only peeks: every byte is still
// left for the frame to read, so nothing about how packets are handled changes.

#include "network.h"

#include <stddef.h>
#include <stdint.h>

// Starts looking for the echo of this packet; call before it is sent.
// A watch already running is dropped.
void net_ping_watch_arm(SOCKET sock, const uint8_t* packet, size_t length);

// Stops looking, as when the link goes away.
void net_ping_watch_disarm(void);

// Takes the instant the whole echo had arrived, in net_now() seconds.
// Returns 0 when it was not seen, in which case there is no figure to draw.
int net_ping_watch_take(double* arrived_seconds);

#endif /* NET_PING_WATCH_H */
