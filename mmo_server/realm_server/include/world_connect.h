#ifndef WORLD_CONNECT_H
#define WORLD_CONNECT_H

/** @file Expose realm connections and configured world-server discovery. */

#include "types.h"
#include "realm_world_auth.h"
#include "session.h"
#include "tls.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <arpa/inet.h>
#include <sys/socket.h> 
#include <sys/types.h>
#include <unistd.h>
#include <ctype.h>

#include <poll.h>
#include <signal.h>

/** Load the realm's identity and the keys it will accept from worlds.
 *
 * The realm↔world link is mutually authenticated TLS: the realm presents the
 * same certificate it serves clients with, and each side checks the other
 * against a pin. Nothing here has a CA to chain to, so a pin is the only thing
 * that distinguishes a world from anything else listening on that port.
 *
 * Startup fails when either file is missing, rather than falling back to
 * plaintext: this link carries the server auth key, and a silent downgrade
 * would leave an operator believing it did not.
 *
 * @return 1 when the realm can open world connections, otherwise 0.
 */
int world_connect_tls_init(const char* cert_path, const char* key_path,
                           const char* world_pin_path);

/** Release what world_connect_tls_init() loaded. */
void world_connect_tls_cleanup(void);

/** Connect to a world, complete a mutual TLS handshake, and authenticate.
 *
 * Every phase is bounded by `timeout_ms`: the connect itself, the TLS
 * handshake, the wait for the authentication acknowledgement, and each read of
 * it. A blocking connect() with no deadline used to stall this thread for the
 * operating system's full SYN timeout -- around two minutes -- whenever a
 * world's address was filtered rather than refused, which on the single
 * world-monitor thread meant every other world's status went stale behind it.
 *
 * @param out_tls     Receives the session on success. The caller owns it and
 *                    must tls_close() it before closing the descriptor.
 * @param silent      Nonzero to suppress connection diagnostics.
 * @param timeout_ms  Deadline applied to each blocking phase, in milliseconds.
 * @return            The authenticated socket descriptor, or -1 on failure.
 */
int connect_to_world_server(const char* host, int port, const char* world_name,
                            const char* server_key, SSL** out_tls,
                            int silent, int timeout_ms);

/** Populate a world-server array from the loaded world table.
 *
 * The realm's roster and the world→database mapping used to be two files that
 * had to agree; they are one table now, read from worlds.conf.
 *
 * @param servers      Array of at least world_table_count() entries.
 * @param max_servers  Capacity of `servers`.
 * @return             The number of entries written, or -1 when no world table
 *                     could be loaded.
 */
int load_world_servers_from_table(WorldServer* servers, int max_servers);

#endif
