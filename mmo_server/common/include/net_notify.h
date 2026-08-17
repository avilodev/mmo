#ifndef NET_NOTIFY_H
#define NET_NOTIFY_H

#include <stddef.h>
#include <stdint.h>

/** @file Build disconnect and rate-limit packets without sending them. */

// accept a NULL message and return zero for insufficient output space
size_t net_build_disconnect(void* out, size_t out_size,
                            uint8_t reason, const char* message);

// return zero when the output cannot hold RateLimitedPacket
size_t net_build_rate_limited(void* out, size_t out_size,
                              uint8_t rejected_type, uint8_t limit_class,
                              uint16_t retry_after_ms);

// map a DisconnectReason value to stable human-readable text
const char* net_disconnect_reason_text(uint8_t reason);

#endif // NET_NOTIFY_H
