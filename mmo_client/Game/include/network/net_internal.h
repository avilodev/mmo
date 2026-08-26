#ifndef NET_INTERNAL_H
#define NET_INTERNAL_H

/**
 * @file
 * Share the client network context and logging between the network sources.
 *
 * Private to Game/src/network/. Nothing outside that directory should include
 * this — the rest of the game talks to the network through network.h, which is
 * still the whole public surface.
 *
 * This exists because packet dispatch used to be one 1,500-line switch with
 * about seventy cases in a single 3,100-line file: session handshakes, item
 * moves, damage numbers, and party rosters all in one function. The cases are
 * now grouped by domain into net_session.c, net_world.c, net_combat.c, and
 * net_inventory.c, and this header is the small amount of state they share.
 */

#include "network.h"
#include "game_types.h"

#include <stdint.h>

/* --- Logging ------------------------------------------------------------
 *
 * Two levels, routed to the rolling log file.
 *
 * NET_LOG is per-packet trace: it fires on the 20Hz position stream and the
 * 10Hz NPC stream. It is a runtime level now rather than a compile-time one --
 * it used to compile to nothing unless someone built with NET_DEBUG, which
 * meant the detail that explains a disconnect did not exist in any shipped
 * build, and a player whose session dropped could report only that it dropped.
 * Set MMO_CLIENT_LOG_LEVEL=trace to turn it on without rebuilding.
 *
 * NET_WARN is for things that indicate an actual fault -- a malformed packet,
 * a size mismatch, a failed send, an unknown opcode. Those are rare by
 * construction, and silencing them would mean a desynced or truncated stream
 * looks exactly like everything working.
 *
 * Both went to stdout, which a released client does not have. They go to
 * Game/logs/client.log; warnings still reach the console for anyone running
 * from a terminal. See core/client_log.h.
 *
 * The trailing newlines in existing call sites are harmless: client_log()
 * appends one, and a blank line in a log is cheaper than editing several
 * hundred call sites to remove them.
 */

#include "core/client_log.h"

/** Set to 1 to force per-packet logging on regardless of the runtime level. */
#ifndef NET_DEBUG
#define NET_DEBUG 0
#endif

#if NET_DEBUG
#define NET_LOG(...) client_log(CLIENT_LOG_INFO, __FILE__, __LINE__, __VA_ARGS__)
#else
#define NET_LOG(...) CLOG_TRACE(__VA_ARGS__)
#endif

#define NET_WARN(...) CLOG_WARN(__VA_ARGS__)

/** Hold the client socket, reassembly buffer, and lock-protected pending responses. */
typedef struct {
    // Connection
    SOCKET   socket;
    BOOL     initialized;
    BOOL     connected;

    /** Set between sending a connect packet and validating the answer.
     *
     * `connected` has to be TRUE across the handshake, because that is what
     * network_update() reads the socket under, and the acknowledgement can
     * only arrive through it. But "the socket is up" and "this is a session"
     * are different claims, and only the second one licenses acting on a
     * gameplay packet: until the ack has been read and found successful, this
     * client has no character in the world, no slot, and no idea what its own
     * state is. A position broadcast or an inventory update arriving in that
     * window used to be dispatched anyway, into exactly that.
     *
     * While this is set, process_packet() admits the handshake and control
     * opcodes and nothing else. net_connect.c owns it.
     */
    BOOL     handshaking;
    uint32_t account_id;
    uint32_t character_id;

    // TCP stream reassembly
    char recv_buf[65536];
    int  recv_len;

    // Combat / movement
    float last_facing_angle;

    // Ping
    double last_ping_time;
    int    pending_pings;
    double ping_send_time;
    int    ping_ms;

    // Pending server responses — written by recv path, read by callers
    CRITICAL_SECTION response_lock;

    struct { BOOL ready; WorldListResponsePacket       data; } world_list;
    struct { BOOL ready; CharacterListResponsePacket   data; } char_list;
    struct { BOOL ready; RaceListResponsePacket         data; } race_list;
    struct { BOOL ready; FormSwapAckPacket              data; } form_swap;
    struct { BOOL ready; CharacterCreateResponsePacket data; } char_create;
    struct { BOOL ready; CharacterDeleteResponsePacket data; } char_delete;
    struct { BOOL ready; EnterWorldResponsePacket      data; } enter_world;
    struct { BOOL ready; CharacterInfo                 data; } char_data;
    struct { BOOL ready; NPCInteractResponsePacket     data; } npc_interact;
    struct { BOOL ready; DialogueUpdatePacket          data; } dialogue_update;
    struct { BOOL ready; DialogueClosePacket           data; } dialogue_close;
    struct { BOOL ready; WorldConnectAckPacket         data; } world_connect_ack;
    struct { BOOL ready; RealmConnectAckPacket         data; } realm_connect_ack;
    struct { BOOL ready; float x; float y;                  } correction;

    // Last rate-limit rejection from the server. Consumed by the UI so a
    // request that was dropped stops a spinner instead of hanging on a
    // response that is never coming.
    struct {
        BOOL     ready;
        uint8_t  rejected_type;
        uint8_t  limit_class;
        uint16_t retry_after_ms;
    } rate_limit;

    // Why the server closed the connection, when it bothered to say. Without
    // this the client can only report a generic timeout.
    struct {
        BOOL    set;
        uint8_t reason;
        char    message[128];
    } disconnect;
} NetContext;

/** The one client network context, defined in network.c. */
extern NetContext g_net;

/** External game state the packet handlers write into. */
extern GameState* g_current_game;

/* --- The transport ------------------------------------------------------
 *
 * One socket serves both server links, and only one of them is encrypted: the
 * realm hop runs over TLS, the world hop over plain TCP. Every packet the
 * client sends therefore goes through net_send() rather than send(), and the
 * single read in network_update() goes through net_recv(). Which of the two
 * transports is in play is decided in one place, by whether a TLS session is
 * attached.
 *
 * The alternative was thirty-six call sites each deciding for themselves, and a
 * new one added later getting it wrong silently -- a plaintext send on the
 * encrypted link does not fail, it just arrives as garbage the realm closes the
 * connection over.
 */

/** Send one packet on whichever transport the current link uses.
 *
 * @return The number of bytes written, or a negative value on failure. Callers
 *         that compare against the packet size keep working unchanged.
 */
int net_send(const void* buf, int len);

/** Read available bytes on whichever transport the current link uses.
 *
 * @return A positive count, 0 for a closed connection, or a negative value when
 *         there is nothing to read right now (which is the common case).
 */
int net_recv(void* buf, int len);

/** Return the client clock in seconds. */
double net_now(void);

/** Print the local player's attributes on one line. */
void net_log_player_stats(const char* prefix);

/* --- Domain dispatch ----------------------------------------------------
 *
 * Each returns 1 when it handled the opcode and 0 when it did not recognize it.
 * network.c offers a packet to each in turn and warns once if nobody claims it.
 * An opcode therefore belongs to exactly one domain, and adding one means
 * touching only that domain's file.
 */

int net_dispatch_session(uint8_t type, const char* data, int length);
int net_dispatch_world(uint8_t type, const char* data, int length);
int net_dispatch_combat(uint8_t type, const char* data, int length);
int net_dispatch_inventory(uint8_t type, const char* data, int length);

#endif // NET_INTERNAL_H
