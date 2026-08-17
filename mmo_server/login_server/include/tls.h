#ifndef TLS_H
#define TLS_H

/** @file Expose the login server's OpenSSL context and thread-local connection I/O. */

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <sys/types.h>

// return NULL when PEM context initialization fails
SSL_CTX* tls_server_init(const char* cert_path, const char* key_path);

void tls_server_cleanup(SSL_CTX* ctx);

// leave client_fd ownership with the caller on handshake failure
SSL* tls_accept(SSL_CTX* ctx, int client_fd);

// bind one SSL session to the current handler thread
void tls_set_conn(SSL* ssl);

// ignore fd and use the current thread's SSL session
ssize_t tls_send(int fd, const void* buf, size_t len, int flags);
ssize_t tls_recv(int fd, void* buf, size_t len, int flags);

void tls_close(SSL* ssl);

#endif // TLS_H
