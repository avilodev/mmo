#ifndef CONFIG_H
#define CONFIG_H

#include "types.h"
#include "packet_limiter.h"
#include "ip_allowlist.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include <ctype.h>
#include <signal.h>
#include <stdatomic.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/** Hold world-server identity, listener state, and packet-limit overrides. */
typedef struct {
    int tcp_sockfd;

    /** Listener the realm's heartbeat link connects to.
     *
     * A second socket rather than a second protocol on the first. Connection
     * type used to be decided by MSG_PEEK on the first byte of every accepted
     * connection, which put the realm's privileged handshake on the same port
     * as the game: reachable by anyone who could reach a player port, and
     * separated from a player only by a byte comparison and an allowlist
     * applied after the accept.
     *
     * Two listeners make the admission decision the socket itself. A world can
     * bind this one to a private interface, or a firewall can drop it from the
     * public one, and a client that speaks the realm opcode by accident or on
     * purpose never reaches the realm code path at all.
     */
    int realm_sockfd;
    pthread_t realm_accept_thread;

    /** Clear to request shutdown.
     *
     * Atomic because the signal handler writes it while the accept thread and
     * the gameplay loop read it. `int` made that a data race, and writing a
     * plain int from a signal handler is undefined behaviour besides; C11
     * lock-free atomics are the type that is actually safe in both roles.
     */
    _Atomic int running;
    pthread_t accept_thread;

    char server_name[32];
    char region[32];
    char ip[16];
    uint16_t port;

    /** Port the realm connects to, from worlds.conf.
     *
     * Defaults to `port + WORLD_REALM_PORT_OFFSET`; an eighth column in
     * worlds.conf, or `realm_port = N` in the world's own .conf, overrides it.
     * Both ends read the same table, so the realm needs no separate setting. */
    uint16_t realm_port;

    /** Address the realm listener binds to.
     *
     * Empty means every interface, which is what the shipped configuration
     * does because the world and its realm run on one host and the loopback
     * allowlist already covers that. A deployment that separates them should
     * set `realm_bind = <address>` to the private interface rather than
     * relying on the allowlist alone. */
    char realm_bind[46];

    /** This world's identifier, resolved from worlds.conf by name at startup.
     *
     * The realm mints a world-entry ticket naming the world it authorised, and
     * this is what that name is checked against. Zero means the world is not
     * listed in worlds.conf, which startup treats as fatal. */
    uint32_t world_id;

    /** Capacity, from the world's .conf.
     *
     * Enforced at admission in net_loop.c. It used to be parsed and validated
     * and then never consulted, so a world filled to the compiled MAX_PLAYERS
     * regardless of what it was configured for, and the realm's "full" status
     * -- computed from this same number -- never actually blocked entry. */
    uint16_t max_players;
    bool hardcore;

    /** Size the NPC pool for this world; 0 keeps NPC_CAPACITY_DEFAULT.
     *
     * The pool is heap-allocated at startup, so this is a deployment decision
     * rather than a recompile. Set `max_npcs = N` in the world's .conf.
     */
    int max_npcs;

    /** Retain compiled packet budgets for override fields left at zero. */
    PacketLimitOverrides limits;

    /** Addresses permitted to open the realm-to-world handshake.
     *
     * The realm handshake shares the player listener and is recognised by its
     * first byte, so before this existed anyone who could reach the world port
     * could make the server spawn a handler thread for them. Left unset in the
     * .conf, set_config() fills in loopback only: a world and its realm run on
     * one host in the shipped configuration, and a default that accepts the
     * internet is not a default worth having.
     *
     * Configure with one `realm_allow = <address or CIDR>` line per source.
     */
    IpAllowlist realm_allow;

    /** Ceiling on concurrently running realm handler threads.
     *
     * Each handler is a detached thread that blocks for up to 20 seconds before
     * any authentication happens, so an unbounded spawn rate is a thread bomb
     * reachable pre-auth. A world talks to a small, fixed number of realms;
     * REALM_MAX_HANDLERS_DEFAULT covers that with room for reconnect overlap.
     *
     * Configure with `realm_max_handlers = N`.
     */
    int realm_max_handlers;

    /** Whether PACKET_SESSION_LIST_REQUEST is answered at all.
     *
     * Off by default. The request returns a paginated roster of every online
     * player -- id, name, level, race, ping -- to any authenticated client,
     * scoped to nothing. That is a complete, refreshable census of who is
     * playing, and it was reachable by anyone who could log in.
     *
     * Turn it on with `session_list = on` in the world's .conf when a
     * deployment actually wants a public "who" list.
     */
    int session_list_enabled;
} ServerConfig;

/** Default ceiling on concurrent realm handler threads. */
#define REALM_MAX_HANDLERS_DEFAULT 4

/** Hard ceiling the configuration cannot exceed. */
#define REALM_MAX_HANDLERS_LIMIT 64

/** Track aggregate world state shared by client-handler threads. */
typedef struct {
    _Atomic int current_players;  /**< Updated concurrently by client-handler threads. */

} ServerState;

extern ServerConfig g_server;
extern ServerState g_state;

int create_tcp_server_socket(int port);

/** Bind and listen on one address and port.
 *
 * @param bind_addr  Address to bind, or NULL/"" for every interface.
 * @return           The listening descriptor, or -1 on failure.
 */
int create_bound_server_socket(const char* bind_addr, int port);
int set_config(const char* filepath);

#endif