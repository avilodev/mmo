/**
 * @file
 * Manage launcher TLS sessions and framed packet transfers.
 */
#include "tls_client.h"
#include "cert_pin.h"
#include "cert_spki.h"

#include <openssl/evp.h>
#include <openssl/opensslv.h>
#include <openssl/x509.h>

#include <windows.h>
#include <stdio.h>
#include <string.h>

static SSL_CTX*   g_ssl_ctx = NULL;
static CertPinSet g_pins;

/** Relative to Launcher.exe: the public-key fingerprints this build accepts. */
#define TLS_PIN_FILE "certs\\login_pins.txt"

/**
 * Build a path to a file shipped beside Launcher.exe.
 *
 * Resolved from the module path rather than the working directory: the launcher
 * is routinely started from a shortcut, and reading verification material from
 * whatever directory happens to be current is its own vulnerability.
 *
 * @return TRUE when the path was produced.
 */
static BOOL path_beside_exe(const char* relative, char* out, size_t out_size) {
    char exe_path[MAX_PATH];
    DWORD length = GetModuleFileNameA(NULL, exe_path, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return FALSE;

    char* slash = strrchr(exe_path, '\\');
    if (!slash) return FALSE;
    *slash = '\0';

    return snprintf(out, out_size, "%s\\%s", exe_path, relative) > 0;
}

/**
 * Initialize the process-wide launcher TLS context.
 *
 * Requires TLS 1.2 and loads the public-key pin set. Initialization fails when
 * no usable pin file is present.
 *
 * Failing closed is the point. This context carries the account password and
 * the 32-byte session key that the realm link then accepts as a credential, and
 * it previously ran with SSL_VERIFY_NONE and no hostname check -- so any peer
 * that could answer on the login port, including one that got there by ARP or
 * DNS, received both in full and could relay the session onward undetected.
 * Chain verification alone cannot fix that: the login certificate is not issued
 * by a public CA, so there is nothing to chain to. A fingerprint of the
 * server's own public key, shipped with the launcher, is what identifies it.
 *
 * @return      TRUE when the context is available, otherwise FALSE.
 */
BOOL tls_client_init(void) {
    if (g_ssl_ctx) return TRUE;

    char pin_path[MAX_PATH * 2];
    if (!path_beside_exe(TLS_PIN_FILE, pin_path, sizeof(pin_path))) {
        fprintf(stderr, "[TLS] Cannot locate the launcher directory\n");
        return FALSE;
    }

    char reason[256];
    if (!cert_pin_load_file(&g_pins, pin_path, reason, sizeof(reason))) {
        fprintf(stderr,
                "[TLS] No usable public-key pins: %s\n"
                "[TLS] The launcher will not connect without them. Generate the file with:\n"
                "[TLS]   openssl x509 -in server.crt -pubkey -noout \\\n"
                "[TLS]     | openssl pkey -pubin -outform der \\\n"
                "[TLS]     | openssl dgst -sha256 -binary | openssl base64\n"
                "[TLS] and write the result as 'sha256/<base64>' into %s\n",
                reason, pin_path);
        return FALSE;
    }

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

    /* Chain verification stays off because there is no chain: the login
     * certificate is self-signed. Identity is decided by the pin check in
     * tls_client_connect(), which runs on every handshake and refuses the
     * connection on a mismatch. Turning SSL_VERIFY_PEER on here without a CA
     * would only fail every connection. */
    SSL_CTX_set_verify(g_ssl_ctx, SSL_VERIFY_NONE, NULL);

    // Require TLS 1.2 minimum.
    SSL_CTX_set_min_proto_version(g_ssl_ctx, TLS1_2_VERSION);

    printf("[TLS] Client context ready (%d pinned key%s)\n",
           g_pins.count, g_pins.count == 1 ? "" : "s");
    return TRUE;
}

/** Release the process-wide TLS context and OpenSSL algorithm state. */
void tls_client_cleanup(void) {
    if (g_ssl_ctx) {
        SSL_CTX_free(g_ssl_ctx);
        g_ssl_ctx = NULL;
    }
    cert_pin_reset(&g_pins);
    EVP_cleanup();
}

/* spki_digest() lived here and did not compute an SPKI digest.
 *
 * It called X509_pubkey_digest(), which hashes the public key BIT STRING
 * without the AlgorithmIdentifier around it -- a different 32 bytes from the
 * SubjectPublicKeyInfo digest that Launcher/certs/login_pins.txt holds and that
 * the openssl pipeline in this file's own error message produces. Every
 * handshake therefore compared one thing against a pin file naming another and
 * was refused, meaning this launcher could not connect to the login server at
 * all. cert_spki.c has the correct computation, shared with the game and both
 * halves of the server tree so there is one of it. */

/**
 * Check a completed handshake against the pin set.
 *
 * @return TRUE when the peer presented a pinned public key.
 */
static BOOL verify_pinned_peer(SSL* ssl) {
    /* Renamed in OpenSSL 3.0; the 1.1 spelling still works there but is
     * deprecated, and the launcher is built against whichever MSYS2 ships. */
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
    X509* cert = SSL_get1_peer_certificate(ssl);
#else
    X509* cert = SSL_get_peer_certificate(ssl);
#endif
    if (!cert) {
        fprintf(stderr, "[TLS] Peer presented no certificate\n");
        return FALSE;
    }

    unsigned char digest[CERT_PIN_DIGEST_LEN];
    BOOL ok = cert_spki_digest(cert, digest) ? TRUE : FALSE;
    X509_free(cert);

    if (!ok) {
        fprintf(stderr, "[TLS] Could not fingerprint the peer public key\n");
        return FALSE;
    }

    if (!cert_pin_matches(&g_pins, digest)) {
        char text[CERT_PIN_DIGEST_LEN * 2 + 1];
        cert_pin_format(digest, text, sizeof(text));
        fprintf(stderr,
                "[TLS] Peer public key %s is not pinned — refusing the connection.\n"
                "[TLS] Either this is not the login server, or its key changed and "
                "the launcher needs an updated pin file.\n", text);
        return FALSE;
    }

    return TRUE;
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

    /* Identity is decided here, before the caller can send anything. A
     * handshake that completed proves only that the peer holds the private key
     * for whatever certificate it offered -- which any interposer also does. */
    if (!verify_pinned_peer(ssl)) {
        SSL_shutdown(ssl);
        SSL_free(ssl);
        return NULL;
    }

    printf("[TLS] Connected — cipher: %s (key pinned)\n", SSL_get_cipher(ssl));
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
