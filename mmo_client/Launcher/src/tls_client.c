#include "tls_client.h"
#include <stdio.h>

static SSL_CTX* g_ssl_ctx = NULL;

BOOL tls_client_init(void) {
    if (g_ssl_ctx) return TRUE;

    // No-ops in OpenSSL 1.1+; kept for 1.0.x compatibility.
    SSL_library_init();
    OpenSSL_add_all_algorithms();
    SSL_load_error_strings();

    const SSL_METHOD* method = TLS_client_method();
    g_ssl_ctx = SSL_CTX_new(method);
    if (!g_ssl_ctx) {
        fprintf(stderr, "[TLS] Failed to create client SSL_CTX\n");
        ERR_print_errors_fp(stderr);
        return FALSE;
    }

    // Skip server certificate verification.
    // Threat model: passive sniffing on LAN/internet.
    // MITM is out of scope for a private game server.
    SSL_CTX_set_verify(g_ssl_ctx, SSL_VERIFY_NONE, NULL);

    // Require TLS 1.2 minimum.
    SSL_CTX_set_min_proto_version(g_ssl_ctx, TLS1_2_VERSION);

    printf("[TLS] Client context ready\n");
    return TRUE;
}

void tls_client_cleanup(void) {
    if (g_ssl_ctx) {
        SSL_CTX_free(g_ssl_ctx);
        g_ssl_ctx = NULL;
    }
    EVP_cleanup();
}

SSL* tls_client_connect(SOCKET sock) {
    if (!g_ssl_ctx) {
        fprintf(stderr, "[TLS] tls_client_connect: context not initialized\n");
        return NULL;
    }

    SSL* ssl = SSL_new(g_ssl_ctx);
    if (!ssl) {
        fprintf(stderr, "[TLS] SSL_new failed\n");
        ERR_print_errors_fp(stderr);
        return NULL;
    }

    // SOCKET is UINT_PTR on 64-bit Windows; cast to int is safe for the
    // small socket descriptor values Windows uses in practice.
    SSL_set_fd(ssl, (int)sock);

    if (SSL_connect(ssl) <= 0) {
        fprintf(stderr, "[TLS] SSL_connect failed\n");
        ERR_print_errors_fp(stderr);
        SSL_free(ssl);
        return NULL;
    }

    printf("[TLS] Connected — cipher: %s\n", SSL_get_cipher(ssl));
    return ssl;
}

int tls_client_send(SSL* ssl, const void* buf, int len) {
    int ret = SSL_write(ssl, buf, len);
    if (ret <= 0) {
        fprintf(stderr, "[TLS] SSL_write error: %d\n", SSL_get_error(ssl, ret));
        ERR_print_errors_fp(stderr);
    }
    return ret;
}

int tls_client_recv(SSL* ssl, void* buf, int len) {
    int ret = SSL_read(ssl, buf, len);
    if (ret <= 0) {
        int err = SSL_get_error(ssl, ret);
        if (err != SSL_ERROR_ZERO_RETURN)
            fprintf(stderr, "[TLS] SSL_read error: %d\n", err);
    }
    return ret;
}

void tls_client_close(SSL* ssl) {
    if (!ssl) return;
    SSL_shutdown(ssl);
    SSL_free(ssl);
}
