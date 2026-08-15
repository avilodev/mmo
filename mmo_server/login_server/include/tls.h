#ifndef TLS_H
#define TLS_H

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <sys/types.h>

// Initialize OpenSSL and create a TLS server context from PEM cert/key files.
// Returns NULL on failure (cert missing, key mismatch, etc.).
SSL_CTX* tls_server_init(const char* cert_path, const char* key_path);

// Free the server context.
void tls_server_cleanup(SSL_CTX* ctx);

// Perform the TLS handshake on an accepted client_fd.
// Returns NULL if the handshake fails — caller should close(client_fd).
SSL* tls_accept(SSL_CTX* ctx, int client_fd);

// Store the SSL* for the current handler thread.
// Call this once at the top of each client_handler_thread.
void tls_set_conn(SSL* ssl);

// Drop-in replacements for send()/recv().
// The fd parameter is kept for signature compatibility but ignored —
// all I/O goes through the thread-local SSL*.
ssize_t tls_send(int fd, const void* buf, size_t len, int flags);
ssize_t tls_recv(int fd, void* buf, size_t len, int flags);

// Cleanly shut down and free the TLS session.
void tls_close(SSL* ssl);

#endif // TLS_H
