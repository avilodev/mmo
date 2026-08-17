/**
 * @file
 * Manage launcher TLS sessions and framed packet transfers.
 */
#include "tls_client.h"
#include <stdio.h>
#include <string.h>

static SSL_CTX* g_ssl_ctx = NULL;

/**
 * Initialize the process-wide launcher TLS context.
 *
 * Server certificate verification is disabled and TLS 1.2 is the minimum protocol version.
 *
 * @return      TRUE when the context is available, otherwise FALSE.
 */
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

    // certificate verification is disabled for this client
    SSL_CTX_set_verify(g_ssl_ctx, SSL_VERIFY_NONE, NULL);

    // Require TLS 1.2 minimum.
    SSL_CTX_set_min_proto_version(g_ssl_ctx, TLS1_2_VERSION);

    printf("[TLS] Client context ready\n");
    return TRUE;
}

/** Release the process-wide TLS context and OpenSSL algorithm state. */
void tls_client_cleanup(void) {
    if (g_ssl_ctx) {
        SSL_CTX_free(g_ssl_ctx);
        g_ssl_ctx = NULL;
    }
    EVP_cleanup();
}

/**
 * Perform a TLS handshake over an already connected socket.
 *
 * The caller retains ownership of the socket and must close it separately.
 *
 * @return      A connected TLS session, or NULL when setup or negotiation fails.
 */
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

/**
 * Write a complete buffer through a TLS session.
 *
 * @return      The bytes written, or the TLS error result when no bytes were written.
 */
int tls_client_send(SSL* ssl, const void* buf, int len) {
    const unsigned char* ptr = (const unsigned char*)buf;
    int total = 0;
    while (total < len) {
        int ret = SSL_write(ssl, ptr + total, len - total);
        if (ret <= 0) {
            fprintf(stderr, "[TLS] SSL_write error: %d\n", SSL_get_error(ssl, ret));
            ERR_print_errors_fp(stderr);
            return total > 0 ? total : ret;
        }
        total += ret;
    }
    return total;
}

/**
 * Read an exact byte count from a TLS session unless an error occurs.
 *
 * @return      The bytes read, or the TLS error result when no bytes were read.
 */
static int tls_recv_exact(SSL* ssl, unsigned char* buf, int len) {
    int total = 0;
    while (total < len) {
        int ret = SSL_read(ssl, buf + total, len - total);
        if (ret <= 0) {
            int err = SSL_get_error(ssl, ret);
            if (err != SSL_ERROR_ZERO_RETURN)
                fprintf(stderr, "[TLS] SSL_read error: %d\n", err);
            return total > 0 ? total : ret;
        }
        total += ret;
    }
    return total;
}

/**
 * Receive one complete MMO packet framed by its seven-byte header.
 *
 * The payload length at header offset five is encoded in network byte order.
 *
 * @return      The complete packet size, a partial header result, or -1 for invalid input, overflow, or a truncated payload.
 */
int tls_client_recv_packet(SSL* ssl, void* buf, int capacity) {
    enum { MMO_HEADER_SIZE = 7 };
    if (!ssl || !buf || capacity < MMO_HEADER_SIZE) return -1;

    unsigned char* bytes = (unsigned char*)buf;
    int got = tls_recv_exact(ssl, bytes, MMO_HEADER_SIZE);
    if (got != MMO_HEADER_SIZE) return got;

    uint16_t payload_net;
    memcpy(&payload_net, bytes + 5, sizeof(payload_net));
    int packet_size = MMO_HEADER_SIZE + (int)ntohs(payload_net);
    if (packet_size > capacity) {
        fprintf(stderr, "[TLS] Packet too large: %d > %d\n", packet_size, capacity);
        return -1;
    }

    got = tls_recv_exact(ssl, bytes + MMO_HEADER_SIZE,
                         packet_size - MMO_HEADER_SIZE);
    if (got != packet_size - MMO_HEADER_SIZE) return -1;
    return packet_size;
}

/**
 * Read up to a requested byte count from a TLS session.
 *
 * @return      The result from SSL_read.
 */
int tls_client_recv(SSL* ssl, void* buf, int len) {
    int ret = SSL_read(ssl, buf, len);
    if (ret <= 0) {
        int err = SSL_get_error(ssl, ret);
        if (err != SSL_ERROR_ZERO_RETURN)
            fprintf(stderr, "[TLS] SSL_read error: %d\n", err);
    }
    return ret;
}

/** Shut down and release a TLS session without closing its socket. */
void tls_client_close(SSL* ssl) {
    if (!ssl) return;
    SSL_shutdown(ssl);
    SSL_free(ssl);
}
