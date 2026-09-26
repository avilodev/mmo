#ifndef TLS_H
#define TLS_H

/** @file The TLS transport shared by every encrypted link in the server tree.
 *
 * Four links are encrypted, and this file serves all four:
 *
 *   client  -> login   TLS, the client pins the login server   (login_net.c)
 *   client  -> realm   TLS, the client pins the realm          (realm_net.c)
 *   realm  <-> world   TLS, mutual: each pins the other        (world_connect.c,
 *                                                               world_server/main.c)
 *   client  -> world   plaintext, deliberately
 *
 * The client->world hop carries positions, damage numbers and chat. Watching it
 * yields what standing next to the player would; it is not worth the per-packet
 * cost on the one link that runs at 20Hz for every player at once. Almost
 * everything worth capturing -- account credentials, session keys, the server
 * auth key, character operations -- travels on one of the other three.
 *
 * The exception is the world-entry ticket. The realm *mints* it over TLS but
 * the client *redeems* it here, in cleartext, so it is readable by anyone on
 * that path. What makes it survivable is that it is single-use (consumed by an
 * atomic fetch-and-delete in Redis), bound to the address the realm issued it
 * to, bound to the world that minted it, and expires in sixty seconds --
 * validate_game_ticket() in session.c enforces all four. README.md, "TLS and
 * pinning", is the operator-facing version of this paragraph; the two are
 * meant to agree.
 *
 * No certificate here is issued by a public CA, so chain verification has
 * nothing to chain to. Identity is a public-key pin on both sides, in
 * cert_pin.h.
 *
 * Two shapes of use, because there are two shapes of link:
 *
 *   Event-driven (login, realm). The descriptor belongs to the reactor and the
 *   handshake is driven across events by tls_handshake_begin/continue. Reads go
 *   through tls_read from the loop; writes go through tls_send from a worker.
 *
 *   Blocking (realm <-> world). One thread owns one connection for its whole
 *   life, so the handshake and the I/O can simply block, bounded by a timeout.
 *   tls_connect_pinned, tls_accept_blocking, tls_send_exact, tls_recv_exact.
 */

#include "cert_pin.h"

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <sys/types.h>

/* --- Contexts ------------------------------------------------------------ */

/** Build a server context from matching PEM certificate and key files.
 *
 * TLS 1.2 is the floor. Peers are not verified: a server that should verify its
 * clients does so by pin, after the handshake, through tls_peer_pin_ok().
 *
 * @return The context, or NULL when setup or credential loading fails.
 */
SSL_CTX* tls_server_init(const char* cert_path, const char* key_path);

/** Build a server context that also demands a client certificate.
 *
 * Used by a world server, whose only peer is a realm. Requesting the
 * certificate is what makes one available to pin; the pin check itself is
 * tls_peer_pin_ok(), because OpenSSL cannot verify what has no chain.
 *
 * @return The context, or NULL when setup or credential loading fails.
 */
SSL_CTX* tls_server_init_mutual(const char* cert_path, const char* key_path);

/** Build a client context, optionally presenting a certificate of its own.
 *
 * @param cert_path  A certificate to present, or NULL to present none.
 * @param key_path   Its key. Required when cert_path is given.
 * @return The context, or NULL when setup or credential loading fails.
 */
SSL_CTX* tls_client_ctx_init(const char* cert_path, const char* key_path);

void tls_server_cleanup(SSL_CTX* ctx);

/* --- Pinning ------------------------------------------------------------- */

/** Fingerprint a session's peer public key.
 *
 * The SubjectPublicKeyInfo rather than the whole certificate, so the pin
 * survives a certificate renewal that keeps the key pair.
 *
 * @param out  Receives CERT_PIN_DIGEST_LEN bytes.
 * @return 1 on success, 0 when the peer presented nothing or it could not be
 *         digested.
 */
int tls_peer_spki_digest(SSL* ssl, unsigned char* out);

/** Check a completed handshake's peer against a pin set.
 *
 * @param who  Named in the log line on a mismatch, e.g. "world Aurora".
 * @return 1 when the peer presented a pinned public key, otherwise 0.
 */
int tls_peer_pin_ok(SSL* ssl, const CertPinSet* pins, const char* who);

/* --- Event-driven links (login, realm) ----------------------------------- */

/** Begin a server handshake on a non-blocking descriptor without waiting.
 *
 * There used to be a blocking tls_accept() beside this, which is what a
 * thread-per-connection server wants: one thread, one handshake, and the
 * thread is free to sit in it. The event loop cannot, because that thread is
 * also every other connection's, so the handshake is started here and driven
 * across events by tls_handshake_continue().
 *
 * @return The session, or NULL when it could not be created.
 */
SSL* tls_handshake_begin(SSL_CTX* ctx, int client_fd);

/** Push a handshake forward as far as it will go right now.
 *
 * @return 1 when the session is ready to carry data, 0 when it needs another
 *         readable or writable event, or -1 when the handshake failed.
 */
int tls_handshake_continue(SSL* ssl);

/** Read from a session, following recv(2)'s conventions.
 *
 * @return A positive count, 0 for a closed session, or -1 with errno set --
 *         EAGAIN when the session simply needs another event.
 */
ssize_t tls_read(SSL* ssl, void* buf, size_t len);

/** Bind one SSL session to the current handler thread.
 *
 * The reply paths in the login and realm route tables were written against a
 * bare descriptor and reach their session through this. Bind before dispatch
 * and unbind after: the same worker goes on to serve other connections.
 */
void tls_set_conn(SSL* ssl);

/** Report the session bound to this thread, or NULL. */
SSL* tls_get_conn(void);

/** Write through the calling thread's session. Ignores flags.
 *
 * Deliberately shaped like send(2) so a route table full of
 * send(fd, buf, len, flags) becomes tls_send(fd, buf, len, flags) without any
 * other change.
 *
 * fd is not what selects the session -- the thread-local binding is -- but it
 * is checked against it, and a mismatch is refused rather than written to the
 * bound session. Every route answers its own requester today, so the two always
 * agree; the check is what stops the first route that answers a *different*
 * connection from silently writing to whichever session this worker holds. Use
 * tls_send_on() to write to a session that is not the bound one.
 *
 * @return The bytes written, or -1 -- including when fd names a different
 *         session than the one bound to this thread.
 */
ssize_t tls_send(int fd, const void* buf, size_t len, int flags);

/** Write through a named session, for callers not on a worker thread.
 *
 * The loop thread has no bound session -- it holds many -- so the few places
 * that answer from the loop (an oversized frame, a refused handshake) name the
 * session they mean.
 */
ssize_t tls_send_on(SSL* ssl, const void* buf, size_t len);

/** Report whether a session's record stream was left half-written.
 *
 * OpenSSL requires an SSL_write() that asked for another event to be retried
 * with the same buffer and the same length. A caller that gives up and later
 * writes something else gets SSL_ERROR_SSL -- "bad write retry" -- and the
 * session is finished. Nearly every reply path in this tree ignores what a
 * send returned, exactly as it did against send(2), so "gives up and later
 * writes something else" is the ordinary shape of any route answering more
 * than once on one connection.
 *
 * So a write that gives up marks its session instead of leaving the next one
 * to discover it. Every later write on a marked session fails immediately
 * without touching OpenSSL, and its owner closes the connection when it next
 * looks -- which turns a stalled peer into a disconnect rather than into a
 * protocol error no log line explains.
 *
 * @return Nonzero when the session can no longer be written to.
 */
int tls_write_broken(SSL* ssl);

/* --- Blocking links (realm <-> world) ------------------------------------ */

/** Connect out, handshake, and verify the peer against a pin set.
 *
 * The descriptor must already be connected. On success the session owns the
 * handshake but not the descriptor: the caller still closes it.
 *
 * @param pins        Accepted peer keys. An empty set refuses everything.
 * @param who         Named in log lines.
 * @param timeout_ms  Deadline for the handshake as a whole.
 * @return The session, or NULL when the handshake failed or the peer was not
 *         pinned.
 */
SSL* tls_connect_pinned(SSL_CTX* ctx, int fd, const CertPinSet* pins,
                        const char* who, int timeout_ms);

/** Accept a handshake on a blocking descriptor, verifying the peer by pin.
 *
 * @param pins  Accepted peer keys, or NULL to accept any peer (no client
 *              certificate is then required either).
 * @return The session, or NULL when the handshake failed or the peer was not
 *         pinned.
 */
SSL* tls_accept_pinned(SSL_CTX* ctx, int fd, const CertPinSet* pins,
                       const char* who, int timeout_ms);

/** Write a whole buffer, or fail.
 *
 * @return 1 when every byte was written, otherwise 0.
 */
int tls_send_exact(SSL* ssl, const void* buf, size_t len, int timeout_ms);

/** Read exactly len bytes, or fail.
 *
 * @return 1 when every byte arrived within the deadline, otherwise 0.
 */
int tls_recv_exact(SSL* ssl, void* buf, size_t len, int timeout_ms);

/** Report whether a session has bytes buffered inside OpenSSL.
 *
 * A TLS record carries many protocol packets, and poll() reports the
 * descriptor, not the record. A caller that polls before every read will park
 * on an idle socket while a whole packet sits decrypted in the session -- so
 * check this first.
 */
int tls_pending(SSL* ssl);

/** Shut down a session and release it. */
void tls_close(SSL* ssl);

#endif // TLS_H
