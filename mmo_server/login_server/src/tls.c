/**
 * @file
 * Manage login-server TLS contexts and thread-local client sessions.
 */
#include "tls.h"

#include <stdio.h>
#include <openssl/ssl.h>
#include <openssl/err.h>

// One SSL* per handler thread, set via tls_set_conn().
static __thread SSL* g_tls_ssl = NULL;

/**
 * Create a TLS server context from matching PEM certificate and key files.
 *
 * TLS versions below 1.2 are rejected.
 *
 * @return      The initialized context, or NULL when setup or credential loading fails.
 */
SSL_CTX* tls_server_init(const char* cert_path, const char* key_path) {
    // In OpenSSL 1.1+ these are no-ops, kept for 1.0.x compatibility.
    SSL_library_init();
    OpenSSL_add_all_algorithms();
    SSL_load_error_strings();

    const SSL_METHOD* method = TLS_server_method();
    SSL_CTX* ctx = SSL_CTX_new(method);
    if (!ctx) {
        fprintf(stderr, "[TLS] Failed to create SSL_CTX\n");
        ERR_print_errors_fp(stderr);
        return NULL;
    }

    // Reject anything below TLS 1.2.
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);

    if (SSL_CTX_use_certificate_file(ctx, cert_path, SSL_FILETYPE_PEM) <= 0) {
        fprintf(stderr, "[TLS] Failed to load certificate: %s\n", cert_path);
        ERR_print_errors_fp(stderr);
        SSL_CTX_free(ctx);
        return NULL;
    }

    if (SSL_CTX_use_PrivateKey_file(ctx, key_path, SSL_FILETYPE_PEM) <= 0) {
        fprintf(stderr, "[TLS] Failed to load private key: %s\n", key_path);
        ERR_print_errors_fp(stderr);
        SSL_CTX_free(ctx);
        return NULL;
    }

    if (!SSL_CTX_check_private_key(ctx)) {
        fprintf(stderr, "[TLS] Certificate and private key do not match\n");
        SSL_CTX_free(ctx);
        return NULL;
    }

    printf("[TLS] Server context ready (TLS 1.2+)\n");
    return ctx;
}

/** Release a TLS server context and OpenSSL algorithm state. */
void tls_server_cleanup(SSL_CTX* ctx) {
    if (ctx) SSL_CTX_free(ctx);
    EVP_cleanup();
}

/**
 * Perform a server-side TLS handshake on an accepted descriptor.
 *
 * The caller retains ownership of client_fd and must close it separately.
 *
 * @return      The negotiated session, or NULL when allocation or negotiation fails.
 */
SSL* tls_accept(SSL_CTX* ctx, int client_fd) {
    SSL* ssl = SSL_new(ctx);
    if (!ssl) {
        fprintf(stderr, "[TLS] SSL_new failed for fd %d\n", client_fd);
        ERR_print_errors_fp(stderr);
        return NULL;
    }

    SSL_set_fd(ssl, client_fd);

    if (SSL_accept(ssl) <= 0) {
        fprintf(stderr, "[TLS] Handshake failed for fd %d\n", client_fd);
        ERR_print_errors_fp(stderr);
        SSL_free(ssl);
        return NULL;
    }

    printf("[TLS] Handshake OK fd=%d cipher=%s\n", client_fd, SSL_get_cipher(ssl));
    return ssl;
}

/** Associate a TLS session with the calling handler thread. */
void tls_set_conn(SSL* ssl) {
    g_tls_ssl = ssl;
}

/**
 * Write through the calling thread's TLS session.
 *
 * @return      The SSL_write result, or -1 when no session is associated with the thread.
 */
ssize_t tls_send(int fd, const void* buf, size_t len, int flags) {
    (void)fd; (void)flags;
    if (!g_tls_ssl) {
        fprintf(stderr, "[TLS] tls_send: no SSL context on this thread\n");
        return -1;
    }
    int ret = SSL_write(g_tls_ssl, buf, (int)len);
    if (ret <= 0) {
        fprintf(stderr, "[TLS] SSL_write error: %d\n", SSL_get_error(g_tls_ssl, ret));
        ERR_print_errors_fp(stderr);
    }
    return (ssize_t)ret;
}

/**
 * Read through the calling thread's TLS session.
 *
 * @return      The SSL_read result, or -1 when no session is associated with the thread.
 */
ssize_t tls_recv(int fd, void* buf, size_t len, int flags) {
    (void)fd; (void)flags;
    if (!g_tls_ssl) {
        fprintf(stderr, "[TLS] tls_recv: no SSL context on this thread\n");
        return -1;
    }
    int ret = SSL_read(g_tls_ssl, buf, (int)len);
    if (ret <= 0) {
        int err = SSL_get_error(g_tls_ssl, ret);
        if (err != SSL_ERROR_ZERO_RETURN)
            fprintf(stderr, "[TLS] SSL_read error: %d\n", err);
    }
    return (ssize_t)ret;
}

/** Shut down a TLS session, release it, and clear the thread-local association. */
void tls_close(SSL* ssl) {
    if (!ssl) return;
    SSL_shutdown(ssl);
    SSL_free(ssl);
    g_tls_ssl = NULL;
}
