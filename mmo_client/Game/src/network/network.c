/**
 * @file
 * Manage client TCP sessions, packet framing, protocol dispatch, and pending responses.
 */

#include "ability_bar.h"
#include "net_internal.h"
#include "network/net_tls.h"
#include "network.h"
#include "game_types.h"
#include "combat_system.h"
#include "combat_render.h"
#include "inventory.h"
#include "npc_types.h"
#include "audio/audio.h"
#include "ui/quest_log.h"

#include <stdio.h>
#include <string.h>
#include <stddef.h>
#include <math.h>
#include <GLFW/glfw3.h>

/** The one client network context. Declared in net_internal.h for the
 * per-domain dispatchers; defined here, where its lifetime is managed. */
NetContext g_net;

// Ping tracking
#define PING_INTERVAL    10.0
#define PING_MAX_MISSED  3      // Disconnect after 3 unanswered pings (~30s)

// External game state for combat events
extern GameState* g_current_game;

/**
 * Return the client clock in seconds.
 */
double net_now(void) {
    return glfwGetTime();
}

#if NET_DEBUG
/** Name each attribute for logging, in StatId order. */
static const char* const k_stat_labels[STAT_COUNT] = {
    "STR", "DEX", "VIT", "INT", "FOC", "END", "FER", "STA", "PRE", "FRL", "ARM"
};
#endif

/**
 * Print the local player's attributes on one line.
 *
 * Driven by the stat array rather than named fields, so a new stat appears in the log
 * as soon as it exists on the wire.
 */
void net_log_player_stats(const char* prefix) {
#if !NET_DEBUG
    // Formatting eleven attributes into a string nobody prints is not free, and
    // this is called on every stats packet. Skip the whole body, not just the write.
    (void)prefix;
#else
    if (!g_current_game || !g_current_game->playing) return;

    char line[256];
    size_t used = 0;
    for (int s = 0; s < STAT_COUNT && used < sizeof(line) - 1; s++) {
        int written = snprintf(line + used, sizeof(line) - used, "%s%s=%d",
                               s ? " " : "", k_stat_labels[s],
                               g_current_game->playing->player_stats[s]);
        if (written < 0) break;
        used += (size_t)written;
    }
    NET_LOG("%s: %s\n", prefix, line);
#endif
}

/**
 * Validate one complete protocol packet and offer it to each domain in turn.
 *
 * The dispatch used to be a single switch of about seventy cases spanning
 * roughly 1,500 lines, mixing session handshakes with damage numbers and item
 * moves. The cases now live in net_session.c, net_world.c, net_combat.c, and
 * net_inventory.c; this only decides who gets asked, and in what order.
 *
 * Order is by traffic, not importance: world and combat carry the per-tick
 * streams, so they are asked first and the rarer session and inventory opcodes
 * pay the extra comparisons instead.
 *
 * Response fields are published while holding response_lock; gameplay events
 * update g_current_game directly.
 *
 * @param data  Buffer containing one complete packet.
 * @param length  Available packet length in bytes.
 */
static void process_packet(const char* data, int length) {
    if (length < (int)sizeof(PacketHeader)) {
        NET_WARN("[NET] Packet too small: %d bytes (need at least %zu)\n",
                 length, sizeof(PacketHeader));
        return;
    }

    const PacketHeader* header = (const PacketHeader*)data;
    const uint8_t type = header->type;

    /* Nothing but the handshake and the control opcodes is acted on until the
     * connect acknowledgement has been read and found successful.
     *
     * The socket is marked connected before the ack arrives, because the ack
     * can only arrive through the read path that flag gates -- see the note on
     * `handshaking` in net_internal.h. What that used to mean is that anything
     * the server sent in the window between the connect packet and its answer
     * was dispatched into a session that did not exist yet: a position
     * broadcast moved a player who had no slot, an inventory update wrote
     * slots belonging to whoever was last in this process's memory, and a
     * refused handshake left all of it behind.
     *
     * The allowed set is small and fixed on purpose: the two acknowledgements
     * being waited for, and the two ways a server ends a connection it has not
     * accepted. */
    if (g_net.handshaking) {
        switch (type) {
            case PACKET_REALM_CONNECT_ACK:
            case PACKET_WORLD_CONNECT_ACK:
            case PACKET_DISCONNECT:
            case PACKET_RATE_LIMITED:
                break;
            default:
                NET_WARN("[NET] Packet type %d arrived during the handshake, "
                         "before this is a session — ignored\n", type);
                return;
        }
    }

    if (net_dispatch_world(type, data, length))     return;
    if (net_dispatch_combat(type, data, length))    return;
    if (net_dispatch_session(type, data, length))   return;
    if (net_dispatch_inventory(type, data, length)) return;
    if (net_dispatch_friends(type, data, length))   return;

    NET_WARN("[NET] Unknown packet type: %d (0x%02X)\n", type, type);
}

/**
 * Initialize Winsock and the shared network context.
 *
 * @return      Nonzero on success or when already initialized; otherwise zero.
 */
int network_init(uint32_t account_id) {
    if (g_net.initialized) return 1;

    memset(&g_net, 0, sizeof(g_net));
    InitializeCriticalSection(&g_net.response_lock);
    g_net.socket = INVALID_SOCKET;

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        NET_WARN("[NET] WSAStartup failed\n");
        return 0;
    }

    g_net.account_id = account_id;
    g_net.initialized = TRUE;
    g_net.last_ping_time = net_now();

    NET_LOG("[NET] Initialized (account %u)\n", account_id);
    return 1;
}

/**
 * Close the socket and release Winsock and synchronization resources.
 */
void network_cleanup(void) {
    /* Releases the context and the pin set as well as any live session: this
     * is the process going away, not one connection ending. */
    net_tls_shutdown();
    net_send_queue_reset();

    if (g_net.socket != INVALID_SOCKET) {
        closesocket(g_net.socket);
        g_net.socket = INVALID_SOCKET;
    }

    if (g_net.initialized) {
        WSACleanup();
        g_net.initialized = FALSE;
    }

    g_net.connected = FALSE;
    g_net.recv_len = 0;
    DeleteCriticalSection(&g_net.response_lock);
}

/**
 * Send a logout when connected, close the socket, and clear session responses.
 */
void network_disconnect(void) {
    if (g_net.socket != INVALID_SOCKET) {
        if (g_net.connected) {
            // Send a clean logout before closing so the server can save immediately
            PacketHeader pkt;
            memset(&pkt, 0, sizeof(pkt));
            pkt.type = PACKET_LOGOUT;
            pkt.player_id = htonl(g_net.account_id);
            pkt.payload_size = 0;
            net_send(&pkt, (int)sizeof(pkt));
        }
        /* After the logout, so it goes out encrypted on a realm link, and
         * before the close, so the session never outlives its descriptor. */
        net_tls_close();
        net_send_queue_reset();
        closesocket(g_net.socket);
        g_net.socket = INVALID_SOCKET;
    }
    g_net.connected = FALSE;
    g_net.pending_pings = 0;
    g_net.recv_len = 0;
    g_net.character_id = 0;
    g_net.last_facing_angle = 0.0f;
    EnterCriticalSection(&g_net.response_lock);
    g_net.rate_limit.ready = FALSE;
    g_net.world_list.ready = FALSE;
    g_net.char_list.ready = FALSE;
    g_net.char_create.ready = FALSE;
    g_net.char_delete.ready = FALSE;
    g_net.enter_world.ready = FALSE;
    g_net.char_data.ready = FALSE;
    g_net.world_connect_ack.ready = FALSE;
    g_net.realm_connect_ack.ready = FALSE;
    g_net.correction.ready = FALSE;
    LeaveCriticalSection(&g_net.response_lock);
}

/**
 * Check whether the network context currently has an accepted connection.
 *
 * @return      Nonzero while connected; otherwise zero.
 */
int network_is_connected(void) {
    return g_net.connected;
}

/**
 * Consume the most recent server rate-limit notice.
 *
 * @param out_type  Optional destination for the rejected packet type.
 * @param out_retry_ms  Optional destination for the retry delay in milliseconds.
 * @return      Nonzero when a notice was consumed; otherwise zero.
 */
int network_get_rate_limit_notice(uint8_t* out_type, uint16_t* out_retry_ms) {
    EnterCriticalSection(&g_net.response_lock);
    if (!g_net.rate_limit.ready) {
        LeaveCriticalSection(&g_net.response_lock);
        return 0;
    }
    if (out_type)     *out_type     = g_net.rate_limit.rejected_type;
    if (out_retry_ms) *out_retry_ms = g_net.rate_limit.retry_after_ms;
    g_net.rate_limit.ready = FALSE;
    LeaveCriticalSection(&g_net.response_lock);
    return 1;
}

/**
 * Copy the retained connection-termination reason.
 *
 * The reason survives disconnect so callers can query it after socket closure.
 *
 * @param out_reason  Optional destination for the protocol reason code.
 * @param out_message  Optional destination for a NUL-terminated message.
 * @param message_size  Capacity of out_message in bytes.
 * @return      Nonzero when a reason is available; otherwise zero.
 */
int network_get_disconnect_reason(uint8_t* out_reason, char* out_message, int message_size) {
    EnterCriticalSection(&g_net.response_lock);
    if (!g_net.disconnect.set) {
        LeaveCriticalSection(&g_net.response_lock);
        return 0;
    }
    if (out_reason) *out_reason = g_net.disconnect.reason;
    if (out_message && message_size > 0) {
        strncpy(out_message, g_net.disconnect.message, (size_t)message_size - 1);
        out_message[message_size - 1] = '\0';
    }
    LeaveCriticalSection(&g_net.response_lock);
    return 1;
}

/**
 * Clear the retained connection-termination reason.
 */
void network_clear_disconnect_reason(void) {
    EnterCriticalSection(&g_net.response_lock);
    g_net.disconnect.set = FALSE;
    g_net.disconnect.message[0] = '\0';
    LeaveCriticalSection(&g_net.response_lock);
}

/**
 * Set the character identifier used in world-server packet headers.
 */
void network_set_character_id(uint32_t character_id) {
    g_net.character_id = character_id;
    NET_LOG("[NET] Character ID set to %u\n", character_id);
}

/* get_packet_size() lived here: a 90-case table mapping every opcode to its
 * fixed wire size, carried since before the dispatchers existed and called by
 * nothing. It survived -Werror only because it was marked
 * __attribute__((unused)), which is the tell -- a second, unenforced copy of
 * the wire contract that no compiler and no test would catch drifting from
 * protocol.h. Each handler in net_session.c, net_world.c, net_combat.c and
 * net_social.c validates its own length against the struct it is about to
 * read, which is the check that actually runs. */

/* --- The transport ------------------------------------------------------- */

/* --- The plaintext send queue -------------------------------------------
 *
 * The world link is not encrypted and so has none of net_tls.c's machinery
 * behind it, and net_send() was a bare send(2) on a non-blocking socket whose
 * return every caller compared against the packet size. Neither of the two
 * things that can go wrong there was handled: a short write left the tail of a
 * packet unsent, and EWOULDBLOCK dropped the packet outright. The first is the
 * serious one -- the world reads the next packet's leading bytes as this one's
 * payload, and the framing is wrong for the rest of the session.
 *
 * So the plaintext link gets the same contract as the encrypted one: bytes the
 * socket will not take are queued and finished by the next flush, and the
 * caller is told the packet was accepted, because it was.
 */

/** Bytes that may wait for a socket that is not taking writes right now.
 *
 * Larger than net_tls.c's queue, because this link carries gameplay rather
 * than a control channel: movement at 20Hz, ability casts, chat. It is sized
 * to hold a short burst of whole packets rather than a fragment of one, and a
 * link that manages to fill it is not busy but stuck -- which is why
 * overflowing is treated as a broken session rather than grown.
 */
#define RAW_PENDING_CAP 32768

static unsigned char g_raw_pending[RAW_PENDING_CAP];
static int           g_raw_pending_len = 0;
/** Set once the byte stream is half-written and nothing more can be sent. */
static int           g_raw_write_broken = 0;

void net_send_queue_reset(void) {
    g_raw_pending_len  = 0;
    g_raw_write_broken = 0;
}

/** Report whether the last socket error was the ordinary "try again". */
static int raw_would_block(void) {
    return WSAGetLastError() == WSAEWOULDBLOCK;
}

/** Append bytes behind whatever is already waiting.
 *
 * @return 1 when they fit, 0 when the queue is full and the link is finished.
 */
static int raw_enqueue(const void* buf, int len) {
    if (len < 0 || len > RAW_PENDING_CAP - g_raw_pending_len) {
        NET_WARN("[NET] The world link stopped accepting writes\n");
        g_raw_write_broken = 1;
        return 0;
    }
    memcpy(g_raw_pending + g_raw_pending_len, buf, (size_t)len);
    g_raw_pending_len += len;
    return 1;
}

/** Drain the queue as far as the socket allows. */
static int raw_flush(void) {
    if (g_raw_write_broken) return 0;
    if (g_net.socket == INVALID_SOCKET) return 1;

    while (g_raw_pending_len > 0) {
        int written = (int)send(g_net.socket, (const char*)g_raw_pending,
                                g_raw_pending_len, 0);
        if (written > 0) {
            g_raw_pending_len -= written;
            if (g_raw_pending_len > 0)
                memmove(g_raw_pending, g_raw_pending + written,
                        (size_t)g_raw_pending_len);
            continue;
        }
        if (written < 0 && raw_would_block()) return 1;   /* still queued */

        NET_WARN("[NET] World link write failed\n");
        g_raw_write_broken = 1;
        return 0;
    }
    return 1;
}

/** Send one packet on the plaintext link, queueing whatever will not go now. */
static int raw_send(const void* buf, int len) {
    if (g_raw_write_broken) return -1;
    if (!raw_flush()) return -1;

    /* Anything still queued means the socket is not taking writes. Going around
     * the queue here would put this packet on the wire ahead of one the world
     * is already half-way through reading. */
    if (g_raw_pending_len > 0)
        return raw_enqueue(buf, len) ? len : -1;

    int written = (int)send(g_net.socket, (const char*)buf, len, 0);
    if (written == len) return len;

    /* A short write. The tail has to be queued rather than reported as a
     * failure: the head is already on the wire, and a packet the world has
     * begun reading cannot be un-sent. */
    if (written > 0)
        return raw_enqueue((const char*)buf + written, len - written) ? len : -1;

    if (written < 0 && raw_would_block())
        return raw_enqueue(buf, len) ? len : -1;

    NET_WARN("[NET] World link write failed\n");
    g_raw_write_broken = 1;
    return -1;
}

int net_send_flush(void) {
    if (net_tls_active()) return net_tls_flush();
    return raw_flush();
}

int net_send(const void* buf, int len) {
    if (g_net.socket == INVALID_SOCKET) return -1;
    if (net_tls_active()) return net_tls_send(buf, len);
    return raw_send(buf, len);
}

int net_recv(void* buf, int len) {
    if (g_net.socket == INVALID_SOCKET) return -1;
    if (net_tls_active()) return net_tls_recv(buf, len);
    return (int)recv(g_net.socket, (char*)buf, len, 0);
}

/** Tear the link down and say why.
 *
 * Four paths below end the connection and every one of them has to release the
 * session before the descriptor, clear the reassembly buffer, and mark the
 * client offline. They were four copies of those four steps, and the socket
 * error path had already lost one of them.
 */
static void drop_link(const char* why) {
    NET_WARN("[NET] %s\n", why);
    net_tls_close();
    net_send_queue_reset();
    if (g_net.socket != INVALID_SOCKET) {
        closesocket(g_net.socket);
        g_net.socket = INVALID_SOCKET;
    }
    g_net.connected = FALSE;
    g_net.recv_len = 0;
}

/**
 * Receive available bytes and dispatch every complete framed packet.
 *
 * The socket is nonblocking; incomplete packets remain in the reassembly buffer for a later call.
 */
void network_update(void) {
    if (g_net.socket == INVALID_SOCKET || !g_net.connected) return;

    /* Anything the link could not take last frame goes out before we read: a
     * queued request that never leaves is a character screen that never fills
     * in, and on the world link it is a movement packet whose tail the server
     * is still waiting for. Both transports queue now; see net_send(). */
    if (!net_send_flush()) {
        drop_link(net_tls_active() ? "TLS session failed while sending"
                                   : "World link failed while sending");
        return;
    }

    /* Why the link has to come down, recorded rather than acted on.
     *
     * These three paths used to call drop_link() and return from here, which
     * threw away every complete packet already sitting in the reassembly
     * buffer -- drop_link() clears recv_len, and the dispatch loop below never
     * ran. A server that says why it is closing and then closes produces
     * exactly that ordering: the read that delivers PACKET_DISCONNECT is
     * followed by the read that reports the FIN, in this same call. So the
     * one packet whose entire purpose is to explain the disconnect was the one
     * packet guaranteed to be discarded, and every close -- a version refusal,
     * a kick, a shutdown notice, an expired session -- reached the player as
     * "the server closed the connection". The reason is dispatched first now,
     * and the link comes down afterwards. */
    const char* drop_reason = NULL;
    char        drop_reason_buf[64];

    // Read as much as we can into the reassembly buffer
    while (1) {
        int space = (int)sizeof(g_net.recv_buf) - g_net.recv_len;
        if (space <= 0) break;

        int bytes = net_recv(g_net.recv_buf + g_net.recv_len, space);

        if (bytes > 0) {
            g_net.recv_len += bytes;
            NET_LOG("[NET] Received %d bytes, buffer now has %d bytes\n", bytes, g_net.recv_len);
        } else if (bytes == 0) {
            drop_reason = "Server closed connection";
            break;
        } else {
            /* On the TLS link the two negative cases have to be told apart
             * here, because WSAGetLastError() describes the descriptor and a
             * session can be broken while the descriptor under it looks
             * healthy. NET_TLS_AGAIN is the ordinary empty read this loop
             * makes every frame; anything else is a link that has to come
             * down, or the client would poll an empty session forever still
             * believing it was connected. */
            if (net_tls_active()) {
                if (bytes == NET_TLS_AGAIN) break;
                drop_reason = "TLS session failed";
                break;
            }

            int err = WSAGetLastError();
            if (err == WSAEWOULDBLOCK) break;
            snprintf(drop_reason_buf, sizeof(drop_reason_buf), "Socket error: %d", err);
            drop_reason = drop_reason_buf;
            break;
        }
    }

    // Process all complete packets in the buffer
    int offset = 0;
    while (offset < g_net.recv_len) {
        int remaining = g_net.recv_len - offset;

        // Need at least a header
        if (remaining < (int)sizeof(PacketHeader)) {
            NET_LOG("[NET] Not enough bytes for header (have %d, need %zu)\n",
                   remaining, sizeof(PacketHeader));
            break;
        }

        uint8_t pkt_type = (uint8_t)g_net.recv_buf[offset];
        PacketHeader* hdr = (PacketHeader*)(g_net.recv_buf + offset);
        int pkt_size = (int)sizeof(PacketHeader) + (int)ntohs(hdr->payload_size);

        NET_LOG("[NET] Packet type %d (0x%02X) at offset %d — payload_size=%d, total=%d, remaining=%d\n",
               pkt_type, pkt_type, offset, (int)ntohs(hdr->payload_size), pkt_size, remaining);

        // Reject oversized packets (corrupted header guard)
        if (pkt_size > (int)sizeof(g_net.recv_buf)) {
            NET_WARN("[NET] ❌ Packet type %d (0x%02X) claims %d bytes — exceeds buffer, disconnecting\n",
                   pkt_type, pkt_type, pkt_size);
            g_net.connected = FALSE;
            closesocket(g_net.socket);
            g_net.socket = INVALID_SOCKET;
            g_net.recv_len = 0;
            return;
        }

        // Wait for the full packet
        if (remaining < pkt_size) {
            NET_LOG("[NET] Waiting for complete packet type %d (have %d, need %d)\n",
                   pkt_type, remaining, pkt_size);
            break;
        }

        // Process the complete packet
        NET_LOG("[NET] Processing complete packet type %d (%d bytes)\n", pkt_type, pkt_size);
        process_packet(g_net.recv_buf + offset, pkt_size);
        offset += pkt_size;
    }

    // Shift remaining data to start of buffer
    if (offset > 0) {
        if (offset < g_net.recv_len) {
            int leftover = g_net.recv_len - offset;
            NET_LOG("[NET] Shifting %d leftover bytes to start of buffer\n", leftover);
            memmove(g_net.recv_buf, g_net.recv_buf + offset, leftover);
            g_net.recv_len = leftover;
        } else {
            // Processed all data
            g_net.recv_len = 0;
        }
    }

    /* Now that everything the server managed to send has been dispatched. A
     * PACKET_DISCONNECT among it has already recorded its reason, which is
     * what network_get_disconnect_reason() hands to the UI. */
    if (drop_reason) drop_link(drop_reason);
}

/**
 * Process incoming packets and maintain the connection heartbeat.
 *
 * Disconnect after PING_MAX_MISSED unanswered heartbeat intervals.
 */
void network_update_with_ping(int game_mode) {
    network_update();

    if (!g_net.connected) return;

    // Send pings in all connected states
    if (game_mode == GAME_MODE_MAIN_MENU      ||
        game_mode == GAME_MODE_SERVER_LIST     ||
        game_mode == GAME_MODE_CHARACTER_SELECT ||
        game_mode == GAME_MODE_PLAYING) {

        double now = net_now();
        if (now - g_net.last_ping_time >= PING_INTERVAL) {
            // Too many unanswered pings — server is unreachable
            if (g_net.pending_pings >= PING_MAX_MISSED) {
                NET_WARN("[NET] Ping timeout: %d pings unanswered, disconnecting\n",
                       g_net.pending_pings);
                // No PACKET_DISCONNECT arrived, so record the cause ourselves
                // rather than leaving the UI with nothing to show.
                EnterCriticalSection(&g_net.response_lock);
                if (!g_net.disconnect.set) {
                    g_net.disconnect.set    = TRUE;
                    g_net.disconnect.reason = DISCONNECT_REASON_UNKNOWN;
                    strncpy(g_net.disconnect.message,
                            "Connection timed out",
                            sizeof(g_net.disconnect.message) - 1);
                }
                LeaveCriticalSection(&g_net.response_lock);
                network_disconnect();
                return;
            }
            network_send_ping();
            g_net.last_ping_time = now;
        }
    }
}


/**
 * Send a packed heartbeat containing the latest measured latency.
 *
 * The packet is serialized manually because PacketHeader is packed and a containing structure would add padding.
 */
void network_send_ping(void) {
    if (!g_net.connected) return;

    // flat buffer avoids padding after the packed header
    uint8_t buf[9]; // sizeof(PacketHeader)=7 + sizeof(uint16_t)=2, no padding
    PacketHeader* hdr = (PacketHeader*)buf;
    memset(buf, 0, sizeof(buf));
    hdr->type         = PACKET_PING;
    hdr->player_id    = htonl(g_net.account_id);
    hdr->payload_size = htons(sizeof(uint16_t));
    uint16_t pm_net   = htons((uint16_t)g_net.ping_ms);
    memcpy(buf + sizeof(PacketHeader), &pm_net, sizeof(uint16_t));

    /* Counted as outstanding whether or not the transport took it.
     *
     * pending_pings used to advance only on a successful send, while the caller
     * moved last_ping_time regardless -- so a link that had stopped accepting
     * writes never reached PING_MAX_MISSED, and the one timeout built to catch
     * exactly that case could not fire. A ping that could not be sent is the
     * strongest evidence there is that the connection is gone. */
    g_net.pending_pings++;
    if (net_send(buf, (int)sizeof(buf)) == (int)sizeof(buf))
        g_net.ping_send_time = net_now();
}

/**
 * Return the most recent round-trip measurement.
 *
 * @return      Round-trip time in milliseconds.
 */
int network_get_ping_ms(void) {
    return g_net.ping_ms;
}

/* --- Realm and world connection handshakes ---------------------------------
 *
 * These live with the rest of the socket lifecycle rather than with the
 * session packet handlers: they create the socket, drive the connect, and
 * own the descriptor from that point on, exactly like network_init() and
 * network_disconnect() above.
 */

/* network_connect_to_realm() and network_connect_to_world() are gone.
 *
 * Both were blocking: a blocking connect(), then a loop of network_update()
 * and Sleep(10) until an acknowledgement arrived or the timeout expired. Both
 * were called from the frame loop on the render thread, so a realm or world
 * that did not answer froze the window for the whole of it -- and the realm
 * one was retried every five seconds, so it froze repeatedly.
 *
 * net_connect.c does the same handshakes as a state machine that advances one
 * non-blocking step per frame. See net_connect.h.
 */
