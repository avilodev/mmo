#ifndef NET_TLS_H
#define NET_TLS_H

/**
 * @file
 * The game's TLS client, used for the realm link and nothing else.
 *
 * Three of the four links in this system are encrypted, and the game is an end
 * of two of them:
 *
 *   launcher -> login   TLS, pinned. Launcher/src/tls_client.c.
 *   game     -> realm   TLS, pinned. This file.
 *   game     -> world   plaintext, deliberately.
 *
 * The realm hop is encrypted because of what it carries: the session key the
 * login server minted, repeated on every connect, and the whole character
 * roster after it. The world hop is not, because what it carries is positions,
 * damage numbers and chat -- watching it yields roughly what standing next to
 * the player would, and it is the one link running at 20Hz for every player at
 * once.
 *
 * The realm's certificate is self-signed, exactly as the login server's is, so
 * there is no chain to verify and no CA to verify it against. What identifies
 * the realm is a fingerprint of its public key, shipped with the client in
 * Game/certs/realm_pins.txt and checked after every handshake. Without a usable
 * pin file the game does not fall back to plaintext -- it refuses to connect,
 * because a silent downgrade would leave the session key on the wire while
 * everything on screen said the connection was fine.
 *
 * The handshake is driven across frames rather than blocked on. The socket is
 * non-blocking and the render thread cannot afford to wait on it -- which is
 * the same reason the TCP connect itself was moved off that thread; see
 * net_connect.h.
 */

#include "network.h"

#include <stddef.h>

/** Load the pin file and build the client context. Idempotent.
 *
 * @return 1 when the game can open a TLS connection, otherwise 0. A 0 has
 *         already been logged with what is missing and how to produce it.
 */
int net_tls_init(void);

/** Release the context and the pin set. */
void net_tls_shutdown(void);

/** Attach a session to a connected socket, without waiting for the handshake.
 *
 * @return 1 when a session was attached, otherwise 0.
 */
int net_tls_begin(SOCKET sock);

/** Push the handshake forward as far as it will go right now.
 *
 * Includes the pin check: a completed handshake against an unpinned key is
 * reported as a failure, not a success.
 *
 * @return 1 when the session is ready to carry data, 0 to be called again next
 *         frame, or -1 when the handshake failed or the peer was not pinned.
 */
int net_tls_continue(void);

/** Report whether a session is attached, handshaking or established. */
int net_tls_active(void);

/** Describe the last failure, for the connection status line. */
const char* net_tls_message(void);

/** Release the session. Safe when none is attached. */
void net_tls_close(void);

/** Write through the session. Follows send(2)'s return convention.
 *
 * A write the socket cannot take right now is queued rather than dropped. See
 * net_tls_flush() for why it cannot simply be reported as a failed request the
 * way a short send(2) was.
 */
int net_tls_send(const void* buf, int len);

/** Push any queued bytes toward the realm. Call once per frame.
 *
 * OpenSSL requires an SSL_write() that asked for another event to be retried
 * with the same bytes and the same length. Reporting it as a failed request --
 * which is what a short send(2) meant, and what this used to do -- leaves a
 * half-written record in the session, and the next write of anything else
 * returns SSL_ERROR_SSL, "bad write retry". That is not a dropped request; it
 * is a dead realm link, arriving one packet after the one that stalled.
 *
 * So a stalled write is held here instead and re-issued unchanged until it
 * lands. Anything sent behind it queues up in order rather than overtaking it.
 *
 * @return 1 when the queue is empty or draining, 0 when the session is broken
 *         and the connection should be torn down.
 */
int net_tls_flush(void);

/** Nothing to read right now. The common case, every frame. */
#define NET_TLS_AGAIN  (-1)
/** The session is broken and the connection should be torn down. */
#define NET_TLS_ERROR  (-2)

/** Read through the session.
 *
 * @return A positive count, 0 for a peer that shut the session down cleanly,
 *         NET_TLS_AGAIN when there is simply nothing yet, or NET_TLS_ERROR.
 *
 * The two negative cases are separated because the caller must act on them
 * differently and cannot tell them apart itself: WSAGetLastError() describes
 * the descriptor, and a TLS session can be broken while the descriptor under it
 * looks perfectly healthy. Folding them together would leave a client whose
 * realm link had failed looping on an empty read forever, still believing it
 * was connected.
 */
int net_tls_recv(void* buf, int len);

#endif // NET_TLS_H
