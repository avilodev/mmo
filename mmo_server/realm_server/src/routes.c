/**
 * @file
 * Enforce realm packet budgets and dispatch authenticated character requests.
 */
#include "routes.h"
#include "log.h"
#include "packet_limiter.h"
#include "net_notify.h"

#include <sys/socket.h>
#include "tls.h"

/** Send an inline rate-limit response on a realm client socket. */
static void realm_send_rate_limited(int fd, uint8_t rejected_type) {
    uint8_t buf[sizeof(RateLimitedPacket)];
    size_t n = net_build_rate_limited(buf, sizeof(buf), rejected_type,
                                      (uint8_t)packet_limiter_class_of(rejected_type),
                                      packet_limiter_retry_after_ms(fd, rejected_type));
    if (n) tls_send(fd, buf, n, MSG_NOSIGNAL);
}

/** Send an inline protocol disconnect on a realm client socket. */
static void realm_send_disconnect(int fd, uint8_t reason) {
    uint8_t buf[sizeof(DisconnectPacket)];
    size_t n = net_build_disconnect(buf, sizeof(buf), reason, NULL);
    if (n) tls_send(fd, buf, n, MSG_NOSIGNAL);
}

/**
 * Rate-limit and dispatch one complete authenticated realm packet.
 *
 * Character names in mutable request buffers are forced to terminate before dispatch.
 *
 * @return      One after ordinary dispatch, zero for an ignored packet, or -1 when the caller must disconnect.
 */
int process_packet(int client_fd, uint32_t account_id, uint8_t* buffer, ssize_t bytes) {
    /* Debug, not info: these two fire on every packet the realm handles, so at
     * info they were the only thing in the file that grew with the player count
     * -- two lines per request, ahead of any decision worth reading about. The
     * outcome of the packet is logged by its handler; this is the trace you turn
     * on when that outcome does not explain itself. */
    LOG_DEBUG("Processing packet from account %u (fd: %d), %zd bytes",
              account_id, client_fd, bytes);

    if (bytes < (ssize_t)sizeof(PacketHeader)) {
        /* Client-controlled: a peer can send a runt packet as fast as the
         * limiter allows, and this is reached before the limiter runs. */
        LOG_WARN_RL(10, 60, "Packet too small (got %zd bytes, need at least %zu)",
                    bytes, sizeof(PacketHeader));
        return 0;
    }

    PacketHeader* header = (PacketHeader*)buffer;

    LOG_DEBUG("Packet type: %d (0x%02X)", header->type, header->type);

    // Spend the connection's budget before doing any real work. Every opcode
    // below is a database round trip, so a flood has to die here rather than in
    // the connection pool.
    switch (packet_limiter_check(client_fd, header->type)) {
        case PACKET_LIMIT_DROP:
            if (packet_limiter_wants_rejection(header->type))
                realm_send_rate_limited(client_fd, header->type);
            return 0;    // over budget: ignore, keep the connection open
        case PACKET_LIMIT_KICK:
            realm_send_disconnect(client_fd, DISCONNECT_REASON_RATE_LIMIT);
            return -1;   // sustained abuse: caller closes the socket
        case PACKET_LIMIT_ALLOW:
        default:
            break;
    }

    switch (header->type) {
        case PACKET_RACE_LIST_REQUEST: {
            handle_race_list_request(client_fd, account_id);
            break;
        }

        case PACKET_CHARACTER_LIST_REQUEST: {
            if (bytes < (ssize_t)sizeof(CharacterListRequestPacket)) break;
            /* Read through the packet struct, like every other case here, and
             * not as *(uint32_t*)(buffer + sizeof(PacketHeader)).
             *
             * PacketHeader is 7 bytes and protocol.h is #pragma pack(1), so
             * that field sits at an odd offset. Reading it through the packed
             * struct is fine -- the compiler knows the struct is packed and
             * emits an access that tolerates it. Casting the raw buffer to a
             * plain uint32_t* instead asserts 4-byte alignment that is never
             * there: undefined behaviour on any target, a trap or a fixup on
             * the AArch64 targets protocol.h says this ships to, and a
             * sanitizer report on x86. Found by tests/realm_routes_fuzz_test.c. */
            const CharacterListRequestPacket* req = (const CharacterListRequestPacket*)buffer;
            handle_character_list_request(client_fd, account_id, ntohl(req->world_id));
            break;
        }

        case PACKET_CHARACTER_CREATE_REQUEST: {
            if (bytes < (ssize_t)sizeof(CharacterCreateRequestPacket)) break;
            CharacterCreateRequestPacket* req = (CharacterCreateRequestPacket*)buffer;
            req->name[sizeof(req->name) - 1] = '\0';  // ensure null-term before strlen (#17)
            handle_character_create_request(client_fd, account_id, ntohl(req->world_id), req->name, ntohl(req->class_id), ntohl(req->race_id));
            break;
        }

        case PACKET_CHARACTER_DELETE_REQUEST: {
            if (bytes < (ssize_t)sizeof(CharacterDeleteRequestPacket)) break;
            CharacterDeleteRequestPacket* req = (CharacterDeleteRequestPacket*)buffer;
            handle_character_delete_request(client_fd, account_id, ntohl(req->character_id), ntohl(req->world_id));
            break;
        }

        case PACKET_WORLD_LIST_REQUEST: {
            LOG_INFO("World list request received");
            world_send_list(client_fd, account_id);
            break;
        }

        case PACKET_ENTER_WORLD: {
            LOG_INFO("Enter world request received");
            world_enter(client_fd, account_id, buffer, bytes);
            break;
        }

        case PACKET_PING:
            LOG_INFO("Ping received, echoing back");
            // Echo exactly the framed packet.  Sending only the header while
            // retaining payload_size desynchronizes the client's TCP stream.
            tls_send(client_fd, buffer, (size_t)bytes, 0);
            break;

        default:
            LOG_WARN("Unknown packet type: %d", header->type);
            break;
    }

    return 1;
}
