#ifndef TLS_CLIENT_H
#define TLS_CLIENT_H

#include <winsock2.h>
#include <openssl/ssl.h>
#include <openssl/err.h>

// Initialize the global TLS client context. Idempotent — safe to call more
// than once. Returns FALSE on failure. Call once from NetworkInit().
BOOL tls_client_init(void);

// Free the global context. Call from NetworkCleanup().
void tls_client_cleanup(void);

// Perform TLS client handshake over an already-connected socket.
// Returns NULL on failure. The caller still owns sock and must
// closesocket() it after calling tls_client_close().
SSL* tls_client_connect(SOCKET sock);

// Send/receive through TLS. Return values match SSL_write/SSL_read
// (bytes transferred, or <= 0 on error).
int tls_client_send(SSL* ssl, const void* buf, int len);
int tls_client_recv(SSL* ssl, void* buf, int len);

// Shut down and free a TLS session. Does NOT close the socket.
void tls_client_close(SSL* ssl);

#endif // TLS_CLIENT_H
