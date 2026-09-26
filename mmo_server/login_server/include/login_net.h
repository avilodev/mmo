#ifndef LOGIN_NET_H
#define LOGIN_NET_H

/** @file Accept TLS login clients onto the shared reactor.
 *
 * The login server is the front door, which makes it the component that takes
 * the launch-day and patch-day surge: everyone arrives at once, and each
 * arrival costs a TLS handshake before a single byte of protocol is read. Under
 * thread-per-connection that surge became one thread per connecting player,
 * each holding an OpenSSL session, with nothing bounding the total.
 *
 * Here the handshake is driven from an event loop across as many readable and
 * writable events as it needs, and only the credential check -- the part that
 * actually blocks, on PostgreSQL and Redis -- occupies one of a fixed number of
 * workers. The thread count is decided at startup and does not move.
 */

#include <openssl/ssl.h>

/** Start the loops and workers.
 *
 * @param ctx           The server TLS context; must outlive the reactor.
 * @param worker_count  Concurrent credential checks, or 0 for the default.
 *                      The knob exists because the right number is a property
 *                      of the database behind it, not of this code.
 * @return 0 on success, or -1 when the reactor could not start.
 */
int login_net_start(SSL_CTX* ctx, int worker_count);

/** Stop and join everything. Safe before a successful start. */
void login_net_stop(void);

/** Report live client connections.
 *
 * How an operator, or a test, sees the thing this design exists to bound. */
int login_net_connection_count(void);

/** Hand an accepted descriptor to the reactor, which takes ownership. */
void login_net_submit(int fd);

#endif // LOGIN_NET_H
