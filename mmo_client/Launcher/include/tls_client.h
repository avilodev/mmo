#ifndef TLS_CLIENT_H
#define TLS_CLIENT_H

#include <winsock2.h>
#include <openssl/ssl.h>
#include <openssl/err.h>

/** Initialize the idempotent global TLS client context, returning FALSE on failure. */
BOOL tls_client_init(void);

/** Free the global TLS client context during network cleanup. */
void tls_client_cleanup(void);

/** Establish TLS over a connected socket, returning NULL on failure.
 *
 * The caller retains the socket and closes it after tls_client_close().
 */
SSL* tls_client_connect(SOCKET sock);

/** Send bytes with SSL_write return semantics. */
int tls_client_send(SSL* ssl, const void* buf, int len);
/** Receive bytes with SSL_read return semantics. */
int tls_client_recv(SSL* ssl, void* buf, int len);
/** Receive one complete MMO packet with its seven-byte header. */
int tls_client_recv_packet(SSL* ssl, void* buf, int capacity);

/** Shut down and free a TLS session while leaving its socket open. */
void tls_client_close(SSL* ssl);

#endif // TLS_CLIENT_H
