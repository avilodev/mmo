/**
 * @file
 * Teach the generic reactor what a world connection is.
 *
 * The epoll loops, the descriptor table, the blocking pool and the idle sweep
 * all live in common/net_reactor.c now, and know nothing about this game. What
 * is left here is only the world's half of the contract: how a connection
 * authenticates, what a packet is, when a connection has gone quiet, and what
 * has to be saved before it goes away.
 */

#include "net_loop.h"
#include "net_send.h"

#include "ability_handler.h"
#include "config.h"
#include "connection_io.h"
#include "log.h"
#include "net_notify.h"
#include "net_reactor.h"
#include "packet_limiter.h"
#include "friends.h"
#include "list_gate.h"
#include "party.h"
#include "peer_addr.h"
#include "protocol.h"
#include "quest_system.h"
#include "routes.h"
#include "player_session.h"
#include "world_entry_service.h"
#include "types.h"
#include "utils.h"

#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>

/** Blocking pool depth: bounds how many logins and logouts run at once.
 *
 * Only the ends of a world session block -- the ticket check on the way in and
 * the character save on the way out -- so this bounds churn, not steady play.
 *
 * Sized from the database pool rather than picked. A worker here spends part of
 * its time in PostgreSQL and part in Redis and socket writes, so a small
 * multiple keeps the connections busy without a crowd waiting on them; the
 * previous value was eight times the pool, which during a mass disconnect would
 * have put most of the save storm into acquire_connection()'s five-second
 * timeout. A save that times out is a character that silently loses progress,
 * and a mass disconnect is exactly when that would happen. */
#define WORLD_WORKERS_PER_DB_CONNECTION 2
#define AUTH_TIMEOUT_SECS  15     // silence allowed before a client authenticates
#define PING_TIMEOUT_SECS  35     // ~3 missed 10s pings on an active session

/** Hold one world connection's authenticated identity.
 *
 * Everything else a connection needs -- its descriptor, its buffer, which loop
 * owns it, when it last spoke -- belongs to the reactor. This is only what the
 * world adds.
 */
typedef struct {
    uint32_t account_id;
    uint32_t character_id;
    /** Cached slot, validated on acquisition; -1 before authentication. */
    int      player_slot;
    /** Nonzero once the ticket has been accepted and the session published. */
    int      authenticated;
    /** Nonzero when the player logged out or was dropped by the server.
     *
     * Those leave the world at once. Anything else that ends the connection --
     * the peer vanishing, a read error, the idle timeout -- is a drop, and the
     * character is kept standing in the world for the grace period. */
	int      clean_exit;

    /** The player's correlation id, from the world ticket.
     *
     * Kept per connection rather than only in the thread-local, because the
     * thread that admits this player is not the thread that later retires
     * them: workers are pooled, and the teardown -- which is where the save
     * is, and so where the interesting failures are -- would otherwise log
     * under no id at all. */
    char     trace_id[TRACE_ID_LEN];
} WorldConn;

static NetReactor* g_reactor = NULL;

static WorldConn* world_conn(NetReactorConn* conn) {
    return (WorldConn*)net_reactor_conn_user(conn);
}

/* --- Accepting ----------------------------------------------------------- */

/** Start the packet budget before the first byte, so a flood during
 *  authentication is metered like any other traffic. */
static int world_on_accept(NetReactorConn* conn) {
    WorldConn* wc = world_conn(conn);
    wc->player_slot = -1;
    packet_limiter_reset(net_reactor_conn_fd(conn));
    return 0;
}

/* --- Reading ------------------------------------------------------------- */

/**
 * Dispatch every complete packet in the buffer.
 *
 * @param consumed  Receives how many leading bytes were dispatched.
 * @return          NET_REACTOR_KEEP, or NET_REACTOR_CLOSE to drop the client.
 */
static NetReactorVerdict dispatch_packets(NetReactorConn* conn, uint8_t* data,
                                          size_t len, size_t* consumed) {
    WorldConn* wc = world_conn(conn);
    int fd = net_reactor_conn_fd(conn);

    size_t used = 0;

    while (len - used >= sizeof(PacketHeader)) {
        PacketHeader* header = (PacketHeader*)(data + used);
        size_t packet_size = sizeof(PacketHeader) + ntohs(header->payload_size);

        if (packet_size > MAX_PACKET_SIZE) {
            LOG_ERROR("[NET] character %u sent an oversized packet (type=%d, size=%zu) "
                      "— disconnecting", wc->character_id, header->type, packet_size);
            uint8_t dc[sizeof(DisconnectPacket)];
            size_t dn = net_build_disconnect(dc, sizeof(dc), DISCONNECT_REASON_PROTOCOL, NULL);
            if (dn) connection_io_send(fd, dc, dn);
			wc->clean_exit = 1;
            *consumed = len;
            return NET_REACTOR_CLOSE;
        }

        if (len - used < packet_size) break;   /* incomplete; carry it over */

        /* No session bookkeeping here. The reactor's own idle clock is what
         * drives the timeout below; the registry write this used to make took a
         * process-wide exclusive lock and scanned every entry per packet to
         * refresh a field nothing read. */
        int result = process_packet(fd, wc->character_id, wc->player_slot,
                                    (ssize_t)packet_size, data + used);
        used += packet_size;

        if (result == -1) {
            /* Clean logout, or the limiter asked for the socket to close. */
			wc->clean_exit = 1;
            *consumed = used;
            return NET_REACTOR_CLOSE;
        }
    }

    *consumed = used;
    return NET_REACTOR_KEEP;
}

/**
 * Route arriving bytes: an unauthenticated connection is waiting for its ticket.
 */
static NetReactorVerdict world_on_data(NetReactorConn* conn, uint8_t* data,
                                       size_t len, size_t* consumed) {
    if (world_conn(conn)->authenticated) return dispatch_packets(conn, data, len, consumed);

    /* Nothing is consumed: ticket validation and the character load both block,
     * so the packet is left where it is and read by the worker. */
    *consumed = 0;
    if (len < sizeof(WorldConnectPacket)) return NET_REACTOR_KEEP;
    return NET_REACTOR_TO_WORKER;
}

/** Push whatever the broadcast threads queued for this connection. */
static int world_on_writable(NetReactorConn* conn) {
    if (!world_conn(conn)->authenticated) return 0;
    return connection_io_flush(net_reactor_conn_fd(conn)) != 0 ? -1 : 0;
}

/* --- Authenticating ------------------------------------------------------ */

/** Answer a refused connection before it is retired. */
static void reject(NetReactorConn* conn, const char* message) {
    WorldConnectAckPacket response = {0};
    response.header.type = PACKET_WORLD_CONNECT_ACK;
    response.header.payload_size = htons(sizeof(response) - sizeof(PacketHeader));
    response.success = 0;
    strncpy(response.welcome_message, message, sizeof(response.welcome_message) - 1);
    server_send_direct(net_reactor_conn_fd(conn), &response, sizeof(response));
}

/**
 * Validate a ticket, load the character, and publish the session.
 *
 * Runs on a worker: every step of it blocks on Redis or PostgreSQL.
 *
 * @return NET_REACTOR_KEEP once the session is live, or NET_REACTOR_CLOSE.
 */
static NetReactorVerdict world_on_work(NetReactorConn* conn) {
    WorldConn* wc = world_conn(conn);
    int fd = net_reactor_conn_fd(conn);

    size_t len = 0;
    uint8_t* buf = net_reactor_conn_buffer(conn, &len);
    if (len < sizeof(WorldConnectPacket)) return NET_REACTOR_CLOSE;

    WorldConnectPacket* pkt = (WorldConnectPacket*)buf;

    if (pkt->header.type != PACKET_WORLD_CONNECT) {
        /* Pre-authentication and reachable by anyone who can open a socket, so
         * a reconnect loop writes one line per attempt. Rate limited: the
         * hundredth identical rejection tells an operator nothing the first
         * did not, and it costs the log history that does. */
        LOG_WARN_RL(10, 60, "[NET] fd %d opened with packet type %u rather than a ticket",
                    fd, pkt->header.type);
        reject(conn, "Invalid ticket");
        return NET_REACTOR_CLOSE;
    }

    /* Version before ticket. Every field read past this point is interpreted
     * according to a layout the client may not share, so a mismatch has to be
     * answered here rather than diagnosed later as a corrupt ticket. */
    uint16_t client_protocol = ntohs(pkt->protocol_version);
    if (client_protocol != PROTOCOL_VERSION) {
        LOG_WARN_RL(10, 60, "[NET] fd %d speaks protocol %u, this world speaks %u — refusing",
                    fd, client_protocol, (unsigned)PROTOCOL_VERSION);
        reject(conn, "This client is a different version than the server. Please update.");
        return NET_REACTOR_CLOSE;
    }

    /* Wire-supplied and not necessarily terminated.
     *
     * game_ticket is a fixed char[64] with no guarantee of a NUL anywhere in
     * it, and world_entry_redeem() builds a Redis key from it with "%s".
     * snprintf reads its source to the terminator regardless of the output
     * bound, so a client that filled the field walked the read past it, on
     * through character_id and protocol_version and into the rest of the
     * reactor's buffer -- before authentication, by anyone who could open the
     * port. Every other wire string on this path is terminated before use
     * (see the RealmAuthPacket fields in main.c); this one was missed. */
    pkt->game_ticket[sizeof(pkt->game_ticket) - 1] = '\0';

    /* The ticket is bound to the address the realm issued it to. It crosses the
     * network in cleartext, so without this check a captured ticket is a working
     * credential for whoever redeems it first. */
    char peer[PEER_ADDR_MAXLEN] = {0};
    peer_addr_text(fd, peer, sizeof(peer));

    /* Capacity is checked before the ticket is consumed.
     *
     * max_players was parsed from the world's .conf, range-checked, printed in
     * the startup banner -- and then never consulted, so a world filled to the
     * compiled MAX_PLAYERS whatever it was configured for, and the realm's
     * "full" status (computed from this same number) never blocked anybody.
     * Refusing before world_entry_redeem() also means a player turned away
     * keeps a usable ticket for another world rather than spending it here.
     *
     * A signed comparison: current_players is an int and a negative value
     * would otherwise promote to something enormous and admit nobody. */
    int here = atomic_load(&g_state.current_players);
    if (g_server.max_players > 0 && here >= (int)g_server.max_players) {
        LOG_INFO("[NET] refusing fd %d: world is full (%d/%u)",
                 fd, here, (unsigned)g_server.max_players);
        reject(conn, "This world is full. Please choose another.");
        return NET_REACTOR_CLOSE;
    }

    /* The world this ticket was minted for must be this world. The identifier
     * was in the ticket all along and was parsed and discarded here, so a
     * ticket for any world opened a session on every world. */
	uint32_t account_id = 0;
	uint32_t character_id = 0;
	world_entry_result entry = world_entry_redeem(pkt->game_ticket, peer,
	                                              g_server.world_id, &account_id,
	                                              &character_id);

	if(entry == WORLD_ENTRY_BAD_TICKET) {
		reject(conn, "Invalid ticket");
		return NET_REACTOR_CLOSE;
	}

	if(entry == WORLD_ENTRY_NOT_OWNER) {
		reject(conn, "Character ownership verification failed");
		return NET_REACTOR_CLOSE;
	}

	int player_slot = -1;
	int added = 0;
	player_session_result opened = player_session_open(fd, account_id, character_id,
	                                                   &player_slot, &added);

	if(opened == PLAYER_SESSION_BUSY) {
		reject(conn, "Server busy");
		return NET_REACTOR_CLOSE;
	}

	if(opened == PLAYER_SESSION_LOAD_FAILED) {
		reject(conn, "Invalid ticket");
		return NET_REACTOR_CLOSE;
	}

    if (!connection_io_register(fd)) {
        player_session_abort(fd, character_id);
        reject(conn, "Server busy");
        return NET_REACTOR_CLOSE;
    }

    wc->account_id    = account_id;
    wc->character_id  = character_id;
    wc->player_slot   = player_slot;
    /* world_entry_redeem() adopted the id for this thread; keep a copy so
     * the retire path can adopt it again on whichever worker gets it. */
    snprintf(wc->trace_id, sizeof(wc->trace_id), "%s", log_get_trace());
    g_state.current_players++;

    /* Record the live session so the realm refuses to delete this character
     * while it is being played. The mark carries a TTL and is refreshed by the
     * periodic save pass, so a world that crashes does not block deletion. */
    world_entry_mark_live(character_id, g_server.world_id);

    WorldConnectAckPacket response = {0};
    response.header.type = PACKET_WORLD_CONNECT_ACK;
    response.header.player_id = htonl(character_id);
    response.header.payload_size = htons(sizeof(response) - sizeof(PacketHeader));
    response.success = 1;
    strncpy(response.welcome_message, "Welcome to the world!",
            sizeof(response.welcome_message) - 1);

    server_send(fd, &response, sizeof(response));

	// Before the player is ready, so the first nearby lists this client is sent are its first
	list_gates_reset_fd(fd);
	player_session_announce(fd, account_id, character_id, added);

    LOG_INFO("[NET] account %u, character %u entered the world", account_id, character_id);

    /* The ticket is done with. Anything the client coalesced behind it stays
     * buffered and is delivered as soon as the loop takes the connection back. */
    net_reactor_conn_consume(conn, sizeof(WorldConnectPacket));

    /* Last, so the loop can never see a half-published session. */
    wc->authenticated = 1;
    return NET_REACTOR_KEEP;
}

/* --- Leaving ------------------------------------------------------------- */

/**
 * Save and detach the player before the descriptor goes away.
 *
 * Runs on a worker for every connection that closes, authenticated or not.
 */
static void world_on_retire(NetReactorConn* conn) {
    WorldConn* wc = world_conn(conn);
    int      client_fd    = net_reactor_conn_fd(conn);
    uint32_t character_id = wc->character_id;
    uint32_t account_id   = wc->account_id;

    /* Log the teardown under the same correlation id the admission used, so a
     * session's last lines join its first. Adopted from the connection rather
     * than looked up: this runs on a pooled worker that may have been doing
     * anything a moment ago. */
    log_set_trace(wc->trace_id);

    if (wc->authenticated) {
		g_state.current_players--;

		if(wc->clean_exit) {
			player_session_close(client_fd, account_id, character_id);

			// Cleared last, so the character stays live while its save is in flight
			world_entry_clear_live(character_id);
		} else {
			// A drop: the character stays, the sweep clears it if nobody returns
			player_session_linkdead(client_fd, account_id, character_id);
			world_entry_keep_login(account_id);
		}

		LOG_INFO("[NET] account %u, character %u disconnected (fd=%d, %s)",
		         account_id, character_id, client_fd,
		         wc->clean_exit ? "logged out" : "dropped");
    }

    /* Last chance to push a queued disconnect reason out before the socket goes
     * away, so the client can say why rather than inferring it from a timeout. */
    connection_io_flush(client_fd);
}

/** Drop per-descriptor state that must not outlive this connection.
 *
 * The reactor closes the descriptor immediately after this, and the number can
 * be handed straight back to accept(), so nothing keyed on it may be left. */
static void world_on_reap(NetReactorConn* conn) {
    int fd = net_reactor_conn_fd(conn);
    connection_io_unregister(fd);
    packet_limiter_reset(fd);
}

/** Decide when silence has gone on long enough.
 *
 * Two limits, because the two phases mean different things: an unauthenticated
 * socket is one an attacker opened and walked away from, and an authenticated
 * one is a player whose pings have stopped arriving. */
static int world_on_idle(NetReactorConn* conn, long idle_seconds) {
    WorldConn* wc = world_conn(conn);
    int fd = net_reactor_conn_fd(conn);

    long limit = wc->authenticated ? PING_TIMEOUT_SECS : AUTH_TIMEOUT_SECS;
    if (idle_seconds > limit) {
        if (wc->authenticated)
            LOG_INFO("[TIMEOUT] character %u timed out (no data for %lds)",
                     wc->character_id, idle_seconds);
        else
            LOG_INFO("[TIMEOUT] fd=%d never authenticated (%lds) — dropping",
                     fd, idle_seconds);
        return 1;
    }

    /* A send queue that has given up is a connection that cannot be told
     * anything, which is indistinguishable from a dead one. */
    return wc->authenticated && connection_io_failed(fd);
}

/* --- Lifecycle ----------------------------------------------------------- */

int net_loop_start(int worker_count) {
    NetReactorConfig config;
    memset(&config, 0, sizeof(config));

    config.name         = "WORLD";
    /* Derived from the pool that is actually opened, not from the compiled
     * default: $MMO_DB_POOL_SIZE moves it, and a worker count that did not
     * move with it is the failure this derivation exists to prevent. */
    config.worker_count = worker_count > 0
        ? worker_count
        : world_entry_db_pool_size() * WORLD_WORKERS_PER_DB_CONNECTION;
    config.buffer_size  = MAX_PACKET_SIZE * 2;
    config.user_size    = sizeof(WorldConn);

    config.on_accept    = world_on_accept;
    config.on_data      = world_on_data;
    config.on_writable  = world_on_writable;
    config.on_work      = world_on_work;
    config.on_retire    = world_on_retire;
    config.on_reap      = world_on_reap;
    config.on_idle      = world_on_idle;

    g_reactor = net_reactor_start(&config);
    return g_reactor ? 0 : -1;
}

void net_loop_stop(void) {
    net_reactor_stop(g_reactor);
    g_reactor = NULL;
}

void net_loop_submit(int fd) {
    if (!g_reactor) {
        LOG_ERROR("[NET] fd %d submitted before the event loops started", fd);
        return;
    }
    net_reactor_submit(g_reactor, fd);
}

int net_loop_connection_count(void) {
    return net_reactor_connection_count(g_reactor);
}

int net_loop_job_depth(int* out_capacity) {
    return net_reactor_job_depth(g_reactor, out_capacity);
}

int net_loop_count(void) {
    return net_reactor_loop_count(g_reactor);
}
