#ifndef NET_NOTIFY_H
#define NET_NOTIFY_H

#include <stddef.h>
#include <stdint.h>

// ============================================================================
// net_notify.h — Builders for the two packets that explain a server decision to
// the client: why a request was dropped, and why a socket is about to close.
//
// These only build; they do not send. The world server writes through its
// queued-write path and the realm server writes directly, so the send call
// belongs to the caller rather than being guessed at here.
// ============================================================================

// Build a DisconnectPacket. `message` may be NULL, in which case a default
// string for the reason is used. Returns bytes written, or 0 if out is too
// small.
size_t net_build_disconnect(void* out, size_t out_size,
                            uint8_t reason, const char* message);

// Build a RateLimitedPacket describing a dropped request.
// Returns bytes written, or 0 if out is too small.
size_t net_build_rate_limited(void* out, size_t out_size,
                              uint8_t rejected_type, uint8_t limit_class,
                              uint16_t retry_after_ms);

// Human-readable text for a DisconnectReason, for logs and for clients that
// prefer the server's wording.
const char* net_disconnect_reason_text(uint8_t reason);

#endif // NET_NOTIFY_H
