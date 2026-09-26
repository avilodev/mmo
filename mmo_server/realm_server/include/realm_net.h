#ifndef REALM_NET_H
#define REALM_NET_H

/** @file Accept realm clients onto the shared reactor.
 *
 * The realm server is where a player picks and creates characters, so nearly
 * everything it does blocks on PostgreSQL or Redis. It used to answer that by
 * giving every connection its own thread, which works right up until a patch
 * day arrives and every player reconnects at once -- and nothing in that design
 * bounds the total.
 *
 * Here the thread count is fixed at startup. A connection lives on an event
 * loop, and each blocking request occupies one of a fixed number of workers for
 * as long as the database takes. A surge becomes a queue rather than a thread
 * per player.
 *
 * The link is TLS, on the same terms as the login server's: the handshake is
 * driven across loop events, and the client identifies this realm by a pin on
 * its public key rather than by a certificate chain there is no CA to issue.
 * What travels here is the session key on every connect and the whole character
 * roster after it, which is why this hop is encrypted and the world hop is not.
 */

#include <openssl/ssl.h>

/** Start the loops and workers.
 *
 * @param ctx           The server TLS context; must outlive the reactor.
 * @param worker_count  Concurrent database requests, or 0 for the default.
 *                      The knob exists because the right number is a property
 *                      of the database behind it, not of this code.
 * @return 0 on success, or -1 when the reactor could not start.
 */
int realm_net_start(SSL_CTX* ctx, int worker_count);

/** Stop and join everything. Safe before a successful start. */
void realm_net_stop(void);

/** Report live client connections.
 *
 * How an operator, or a test, sees the thing this design exists to bound. */
int realm_net_connection_count(void);

/** Hand an accepted descriptor to the reactor, which takes ownership. */
void realm_net_submit(int fd);

#endif // REALM_NET_H
