/**
 * @file
 * Build protocol notifications for rejected requests and disconnections.
 */

#include "net_notify.h"
#include "protocol.h"

#include <arpa/inet.h>
#include <string.h>

/**
 * Return the default client-facing text for a disconnect reason.
 *
 * @return      A static reason string, including a generic value for unknown reasons.
 */
const char* net_disconnect_reason_text(uint8_t reason) {
    switch (reason) {
        case DISCONNECT_REASON_SHUTDOWN:
            return "Server is shutting down";
        case DISCONNECT_REASON_RATE_LIMIT:
            return "Disconnected: too many packets sent";
        case DISCONNECT_REASON_PROTOCOL:
            return "Disconnected: malformed packet";
        case DISCONNECT_REASON_AUTH:
            return "Disconnected: session invalid or expired";
        case DISCONNECT_REASON_KICKED:
            return "You have been kicked from the server";
        default:
            return "Disconnected by server";
    }
}

/**
 * Build a disconnect notification packet.
 *
 * @param out  Destination buffer for DisconnectPacket.
 * @param out_size  Destination capacity in bytes.
 * @param reason  DisconnectReason value.
 * @param message  Optional message overriding the default reason text.
 * @return      Packet size in bytes, or 0 when the destination is invalid or too small.
 */
size_t net_build_disconnect(void* out, size_t out_size,
                            uint8_t reason, const char* message) {
    if (!out || out_size < sizeof(DisconnectPacket)) return 0;

    DisconnectPacket* pkt = (DisconnectPacket*)out;
    memset(pkt, 0, sizeof(*pkt));

    pkt->header.type         = PACKET_DISCONNECT;
    pkt->header.payload_size = htons(sizeof(DisconnectPacket) - sizeof(PacketHeader));
    pkt->reason              = reason;

    const char* text = message ? message : net_disconnect_reason_text(reason);
    strncpy(pkt->message, text, sizeof(pkt->message) - 1);

    return sizeof(DisconnectPacket);
}

/**
 * Build a rate-limit rejection packet.
 *
 * @param out  Destination buffer for RateLimitedPacket.
 * @param out_size  Destination capacity in bytes.
 * @param rejected_type  Packet opcode that exceeded its limit.
 * @param limit_class  Applied packet-limiter class.
 * @param retry_after_ms  Suggested retry delay in milliseconds.
 * @return      Packet size in bytes, or 0 when the destination is invalid or too small.
 */
size_t net_build_rate_limited(void* out, size_t out_size,
                              uint8_t rejected_type, uint8_t limit_class,
                              uint16_t retry_after_ms) {
    if (!out || out_size < sizeof(RateLimitedPacket)) return 0;

    RateLimitedPacket* pkt = (RateLimitedPacket*)out;
    memset(pkt, 0, sizeof(*pkt));

    pkt->header.type         = PACKET_RATE_LIMITED;
    pkt->header.payload_size = htons(sizeof(RateLimitedPacket) - sizeof(PacketHeader));
    pkt->rejected_type       = rejected_type;
    pkt->limit_class         = limit_class;
    pkt->retry_after_ms      = htons(retry_after_ms);

    return sizeof(RateLimitedPacket);
}
