/**
 * @file
 * Implement the game's TLS client for the realm link.
 */

#include "network/net_tls.h"
#include "net_internal.h"
#include "cert_pin.h"
#include "cert_spki.h"

#include <openssl/ssl.h>
#include <openssl/err.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** Where the accepted realm public keys are read from.
 *
 * Relative to the working directory, matching SETTINGS_PATH and the rolling
 * log. $MMO_REALM_PINS overrides it, which is what a developer running the
 * server on another machine uses.
 */
#define REALM_PIN_PATH_DEFAULT "Game/certs/realm_pins.txt"

static SSL_CTX*   g_ctx = NULL;
static CertPinSet g_pins;
static SSL*       g_ssl = NULL;
static int        g_established = 0;
static char       g_message[192] = "";

/** Record a failure reason and log it once. */
static void fail(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(g_message, sizeof(g_message), fmt, args);
    va_end(args);
    NET_WARN("[TLS] %s\n", g_message);
}

const char* net_tls_message(void) {
    return g_message;
}

int net_tls_init(void) {
    if (g_ctx) return 1;

    const char* pin_path = getenv("MMO_REALM_PINS");
    if (!pin_path || !*pin_path) pin_path = REALM_PIN_PATH_DEFAULT;

    char reason[160];
    cert_pin_reset(&g_pins);
    if (!cert_pin_load_file(&g_pins, pin_path, reason, sizeof(reason))) {
        fail("No usable realm public-key pins in %s: %s", pin_path, reason);
        NET_WARN("[TLS] The game will not connect to a realm without them.\n");
        NET_WARN("[TLS] Run the server's setup/setup.sh, which generates the realm\n");
        NET_WARN("[TLS] certificate and writes this file. By hand, from the realm's\n");
        NET_WARN("[TLS] certs/server.crt:\n");
        NET_WARN("[TLS]   openssl x509 -in server.crt -pubkey -noout \\\n");
        NET_WARN("[TLS]     | openssl pkey -pubin -outform der \\\n");
        NET_WARN("[TLS]     | openssl dgst -sha256 -binary | openssl base64\n");
        NET_WARN("[TLS] and write the result as 'sha256/<base64>' into %s\n", pin_path);
        return 0;
    }

    /* No-ops in OpenSSL 1.1+; kept for 1.0.x compatibility. */
    SSL_library_init();
    OpenSSL_add_all_algorithms();
    SSL_load_error_strings();

    g_ctx = SSL_CTX_new(TLS_client_method());
    if (!g_ctx) {
        fail("Could not create the TLS client context");
        cert_pin_reset(&g_pins);
        return 0;
    }

    /* Chain verification stays off because there is no chain: the realm
     * certificate is self-signed. Identity is decided by the pin check in
     * net_tls_continue(), which runs on every handshake and refuses the
     * connection on a mismatch. Turning SSL_VERIFY_PEER on here without a CA
     * would only fail every connection. */
    SSL_CTX_set_verify(g_ctx, SSL_VERIFY_NONE, NULL);
    SSL_CTX_set_min_proto_version(g_ctx, TLS1_2_VERSION);

    NET_LOG("[TLS] Realm client ready (%d pinned key%s)\n",
            g_pins.count, g_pins.count == 1 ? "" : "s");
    return 1;
}

void net_tls_shutdown(void) {
    net_tls_close();
    if (g_ctx) {
        SSL_CTX_free(g_ctx);
        g_ctx = NULL;
    }
    cert_pin_reset(&g_pins);
}

int net_tls_begin(SOCKET sock) {
    if (!net_tls_init()) return 0;

    net_tls_close();

    g_ssl = SSL_new(g_ctx);
    if (!g_ssl) {
        fail("Could not create a TLS session");
        return 0;
    }

    /* SOCKET is an unsigned handle on Windows and an int everywhere else;
     * OpenSSL wants the int. The cast is safe for any descriptor a client
     * process actually holds. */
    SSL_set_fd(g_ssl, (int)sock);
    g_established = 0;
    return 1;
}

/** Fingerprint the peer's public key and test it against the pin set. */
static int peer_is_pinned(void) {
    unsigned char digest[CERT_PIN_DIGEST_LEN];
    if (!cert_spki_digest_peer(g_ssl, digest)) {
        fail("The realm presented no usable certificate");
        return 0;
    }

    if (cert_pin_matches(&g_pins, digest)) return 1;

    char text[CERT_PIN_DIGEST_LEN * 2 + 1];
    cert_pin_format(digest, text, sizeof(text));
    fail("The realm's key is not pinned — refusing the connection");
    NET_WARN("[TLS] peer key %s\n", text);
    NET_WARN("[TLS] Either this is not the realm server, or its key changed and\n");
    NET_WARN("[TLS] the client needs an updated pin file.\n");
    return 0;
}

int net_tls_continue(void) {
    if (!g_ssl) return -1;
    if (g_established) return 1;

    int ret = SSL_connect(g_ssl);
    if (ret == 1) {
        if (!peer_is_pinned()) return -1;
        g_established = 1;
        NET_LOG("[TLS] Realm handshake complete and pinned\n");
        return 1;
    }

    int err = SSL_get_error(g_ssl, ret);
    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) return 0;

    fail("The TLS handshake with the realm failed");
    return -1;
}

int net_tls_active(void) {
    return g_ssl != NULL;
}

void net_tls_close(void) {
    if (!g_ssl) return;
    /* No SSL_shutdown handshake: the caller is about to close the descriptor
     * either way, and waiting for a close_notify from a realm that may already
     * be gone would block the frame. */
    SSL_free(g_ssl);
    g_ssl = NULL;
    g_established = 0;
}

int net_tls_send(const void* buf, int len) {
    if (!g_ssl || !g_established) return -1;

    int ret = SSL_write(g_ssl, buf, len);
    if (ret > 0) return ret;

    int err = SSL_get_error(g_ssl, ret);
    /* The socket is non-blocking, so a full send buffer surfaces here. The
     * callers all send small fixed packets and treat a short write as a failed
     * request, which is what they did against send() too. */
    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) return -1;

    NET_WARN("[TLS] write failed (SSL error %d)\n", err);
    return -1;
}

int net_tls_recv(void* buf, int len) {
    if (!g_ssl || !g_established) return NET_TLS_ERROR;

    int ret = SSL_read(g_ssl, buf, len);
    if (ret > 0) return ret;

    int err = SSL_get_error(g_ssl, ret);
    if (err == SSL_ERROR_ZERO_RETURN) return 0;      /* clean TLS shutdown */
    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE)
        return NET_TLS_AGAIN;

    NET_WARN("[TLS] read failed (SSL error %d)\n", err);
    return NET_TLS_ERROR;
}
