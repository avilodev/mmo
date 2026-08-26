/**
 * @file
 * Implement the TLS transport shared by every encrypted link in the tree.
 *
 * See tls.h for which links those are and why one of them is not.
 */
#include "tls.h"
#include "cert_spki.h"
#include "log.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <openssl/ssl.h>
#include <openssl/err.h>

/** Bound how long one response may wait on a peer that has stopped reading. */
#define TLS_WRITE_ATTEMPTS    16
#define TLS_WRITE_TIMEOUT_MS 500

// One SSL* per handler thread, set via tls_set_conn().
static __thread SSL* g_tls_ssl = NULL;

/* --- Contexts ------------------------------------------------------------ */

/** Load the shared parts of any context: version floor, certificate, key. */
static SSL_CTX* ctx_new(const SSL_METHOD* method, const char* cert_path,
                        const char* key_path) {
    // In OpenSSL 1.1+ these are no-ops, kept for 1.0.x compatibility.
    SSL_library_init();
    OpenSSL_add_all_algorithms();
    SSL_load_error_strings();

    SSL_CTX* ctx = SSL_CTX_new(method);
    if (!ctx) {
        LOG_ERROR("[TLS] Failed to create SSL_CTX");
        ERR_print_errors_fp(stderr);
        return NULL;
    }

    // Reject anything below TLS 1.2.
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);

    if (!cert_path) return ctx;      /* a client that presents nothing */

    if (SSL_CTX_use_certificate_file(ctx, cert_path, SSL_FILETYPE_PEM) <= 0) {
        LOG_ERROR("[TLS] Failed to load certificate: %s", cert_path);
        ERR_print_errors_fp(stderr);
        SSL_CTX_free(ctx);
        return NULL;
    }

    if (SSL_CTX_use_PrivateKey_file(ctx, key_path, SSL_FILETYPE_PEM) <= 0) {
        LOG_ERROR("[TLS] Failed to load private key: %s", key_path);
        ERR_print_errors_fp(stderr);
        SSL_CTX_free(ctx);
        return NULL;
    }

    if (!SSL_CTX_check_private_key(ctx)) {
        LOG_ERROR("[TLS] Certificate and private key do not match");
        SSL_CTX_free(ctx);
        return NULL;
    }

    return ctx;
}

SSL_CTX* tls_server_init(const char* cert_path, const char* key_path) {
    SSL_CTX* ctx = ctx_new(TLS_server_method(), cert_path, key_path);
    if (!ctx) return NULL;

    /* Peers are not verified by chain, here or anywhere in this tree: every
     * certificate is self-signed, so SSL_VERIFY_PEER without a CA would refuse
     * every connection rather than check anything. Where a peer must be
     * identified it is identified by pin, after the handshake. */
    SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL);

    LOG_INFO("[TLS] Server context ready (TLS 1.2+)");
    return ctx;
}

/** Accept whatever certificate the peer sends; the pin check decides.
 *
 * SSL_VERIFY_PEER on its own would fail the handshake during chain
 * verification, before there is anything to fingerprint. This callback lets
 * the handshake complete so tls_peer_pin_ok() can make the actual decision,
 * which is the only one that means anything without a CA.
 */
static int accept_any_cert(int preverify_ok, X509_STORE_CTX* store) {
    (void)preverify_ok; (void)store;
    return 1;
}

SSL_CTX* tls_server_init_mutual(const char* cert_path, const char* key_path) {
    SSL_CTX* ctx = ctx_new(TLS_server_method(), cert_path, key_path);
    if (!ctx) return NULL;

    /* FAIL_IF_NO_PEER_CERT is the part that matters: a peer that sends no
     * certificate at all has nothing to pin, and must not reach the protocol
     * on the strength of having completed a handshake. */
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT,
                       accept_any_cert);

    LOG_INFO("[TLS] Mutual server context ready (TLS 1.2+, client certificate required)");
    return ctx;
}

SSL_CTX* tls_client_ctx_init(const char* cert_path, const char* key_path) {
    SSL_CTX* ctx = ctx_new(TLS_client_method(), cert_path, key_path);
    if (!ctx) return NULL;

    SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL);
    LOG_INFO("[TLS] Client context ready (TLS 1.2+%s)",
             cert_path ? ", presenting a certificate" : "");
    return ctx;
}

void tls_server_cleanup(SSL_CTX* ctx) {
    if (ctx) SSL_CTX_free(ctx);
    EVP_cleanup();
}

/* --- Pinning ------------------------------------------------------------- */

int tls_peer_spki_digest(SSL* ssl, unsigned char* out) {
    if (!out) return 0;
    return cert_spki_digest_peer(ssl, out);
}

int tls_peer_pin_ok(SSL* ssl, const CertPinSet* pins, const char* who) {
    if (!pins || cert_pin_is_empty(pins)) {
        LOG_ERROR("[TLS] no pins configured for %s — refusing", who ? who : "peer");
        return 0;
    }

    unsigned char digest[CERT_PIN_DIGEST_LEN];
    if (!tls_peer_spki_digest(ssl, digest)) {
        LOG_WARN("[TLS] %s presented no usable certificate", who ? who : "peer");
        return 0;
    }

    if (cert_pin_matches(pins, digest)) return 1;

    char text[CERT_PIN_DIGEST_LEN * 2 + 1];
    cert_pin_format(digest, text, sizeof(text));
    LOG_WARN("[TLS] %s presented public key %s, which is not pinned — refusing. "
             "Either this is not that peer, or its key changed and the pin file "
             "needs updating.",
             who ? who : "peer", text);
    return 0;
}

/* --- Event-driven links -------------------------------------------------- */

SSL* tls_handshake_begin(SSL_CTX* ctx, int client_fd) {
    SSL* ssl = SSL_new(ctx);
    if (!ssl) {
        LOG_ERROR("[TLS] SSL_new failed for fd %d", client_fd);
        ERR_print_errors_fp(stderr);
        return NULL;
    }
    SSL_set_fd(ssl, client_fd);
    return ssl;
}

int tls_handshake_continue(SSL* ssl) {
    if (!ssl) return -1;

    int ret = SSL_accept(ssl);
    if (ret == 1) return 1;

    int err = SSL_get_error(ssl, ret);

    /* Both directions are already armed on the loop, so "wants read" and
     * "wants write" mean the same thing here: come back on the next event. */
    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) return 0;

    /* A client that connects and goes away mid-handshake is ordinary, and
     * scanners do it constantly; it is not worth a stack trace each time. */
    if (err != SSL_ERROR_ZERO_RETURN && err != SSL_ERROR_SYSCALL)
        ERR_print_errors_fp(stderr);
    return -1;
}

ssize_t tls_read(SSL* ssl, void* buf, size_t len) {
    if (!ssl) { errno = EINVAL; return -1; }

    int ret = SSL_read(ssl, buf, (int)len);
    if (ret > 0) return ret;

    int err = SSL_get_error(ssl, ret);
    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
        errno = EAGAIN;
        return -1;
    }
    if (err == SSL_ERROR_ZERO_RETURN) return 0;   /* clean TLS shutdown */

    errno = (err == SSL_ERROR_SYSCALL && errno) ? errno : ECONNRESET;
    return -1;
}

void tls_set_conn(SSL* ssl) {
    g_tls_ssl = ssl;
}

SSL* tls_get_conn(void) {
    return g_tls_ssl;
}

ssize_t tls_send_on(SSL* ssl, const void* buf, size_t len) {
    if (!ssl) {
        LOG_ERROR("[TLS] send on a connection with no session");
        return -1;
    }

    /* The descriptor is non-blocking now that the loop owns it, so a write can
     * come back asking for another event. This runs on a worker, whose whole
     * purpose is to be the thread that may wait, so it waits here -- bounded,
     * so a peer that stops reading cannot hold a worker forever. */
    for (int attempt = 0; attempt < TLS_WRITE_ATTEMPTS; attempt++) {
        int ret = SSL_write(ssl, buf, (int)len);
        if (ret > 0) return (ssize_t)ret;

        int err = SSL_get_error(ssl, ret);
        if (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE) {
            LOG_ERROR("[TLS] SSL_write error: %d", err);
            ERR_print_errors_fp(stderr);
            return (ssize_t)ret;
        }

        struct pollfd pfd = {
            .fd     = SSL_get_fd(ssl),
            .events = (short)(err == SSL_ERROR_WANT_READ ? POLLIN : POLLOUT),
        };
        if (poll(&pfd, 1, TLS_WRITE_TIMEOUT_MS) <= 0) break;
    }

    LOG_ERROR("[TLS] SSL_write gave up waiting for the peer");
    return -1;
}

ssize_t tls_send(int fd, const void* buf, size_t len, int flags) {
    (void)fd; (void)flags;
    if (!g_tls_ssl) {
        LOG_ERROR("[TLS] tls_send: no SSL context on this thread");
        return -1;
    }
    return tls_send_on(g_tls_ssl, buf, len);
}

/* tls_recv() is gone with tls_accept(). Reads come off the reactor's own
 * buffer through tls_read(), which takes the session explicitly rather than
 * finding it in a thread-local -- the reactor's threads each carry many
 * sessions, so there is no such thing as "this thread's" session to read. */

/* --- Blocking links ------------------------------------------------------ */

/** Drive a blocking-socket handshake to completion within a deadline.
 *
 * The descriptor may be in blocking mode, in which case OpenSSL never asks for
 * another event and this returns on the first call. It may equally be
 * non-blocking, which is why the WANT_READ/WANT_WRITE poll is here: the realm
 * connects with a deadline and leaves the descriptor blocking afterwards, but
 * a caller that does not is not thereby broken.
 *
 * @param accepting  Nonzero to run SSL_accept, zero to run SSL_connect.
 * @return 1 on a completed handshake, otherwise 0.
 */
static int handshake_blocking(SSL* ssl, int accepting, int timeout_ms) {
    for (;;) {
        int ret = accepting ? SSL_accept(ssl) : SSL_connect(ssl);
        if (ret == 1) return 1;

        int err = SSL_get_error(ssl, ret);
        if (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE) {
            if (err != SSL_ERROR_ZERO_RETURN && err != SSL_ERROR_SYSCALL)
                ERR_print_errors_fp(stderr);
            return 0;
        }

        struct pollfd pfd = {
            .fd     = SSL_get_fd(ssl),
            .events = (short)(err == SSL_ERROR_WANT_READ ? POLLIN : POLLOUT),
        };
        int ready = poll(&pfd, 1, timeout_ms);
        if (ready < 0 && errno == EINTR) continue;
        if (ready <= 0) return 0;
    }
}

SSL* tls_connect_pinned(SSL_CTX* ctx, int fd, const CertPinSet* pins,
                        const char* who, int timeout_ms) {
    if (!ctx) return NULL;
    if (timeout_ms <= 0) timeout_ms = 5000;

    SSL* ssl = SSL_new(ctx);
    if (!ssl) {
        LOG_ERROR("[TLS] SSL_new failed connecting to %s", who ? who : "peer");
        ERR_print_errors_fp(stderr);
        return NULL;
    }
    SSL_set_fd(ssl, fd);

    if (!handshake_blocking(ssl, 0, timeout_ms)) {
        LOG_WARN("[TLS] handshake with %s did not complete", who ? who : "peer");
        SSL_free(ssl);
        return NULL;
    }

    if (!tls_peer_pin_ok(ssl, pins, who)) {
        SSL_free(ssl);
        return NULL;
    }

    return ssl;
}

SSL* tls_accept_pinned(SSL_CTX* ctx, int fd, const CertPinSet* pins,
                       const char* who, int timeout_ms) {
    if (!ctx) return NULL;
    if (timeout_ms <= 0) timeout_ms = 5000;

    SSL* ssl = SSL_new(ctx);
    if (!ssl) {
        LOG_ERROR("[TLS] SSL_new failed accepting %s", who ? who : "peer");
        ERR_print_errors_fp(stderr);
        return NULL;
    }
    SSL_set_fd(ssl, fd);

    if (!handshake_blocking(ssl, 1, timeout_ms)) {
        LOG_WARN("[TLS] handshake from %s did not complete", who ? who : "peer");
        SSL_free(ssl);
        return NULL;
    }

    if (pins && !tls_peer_pin_ok(ssl, pins, who)) {
        SSL_free(ssl);
        return NULL;
    }

    return ssl;
}

int tls_send_exact(SSL* ssl, const void* buf, size_t len, int timeout_ms) {
    if (!ssl) return 0;
    if (timeout_ms <= 0) timeout_ms = 5000;

    const unsigned char* ptr = buf;
    size_t total = 0;
    while (total < len) {
        int ret = SSL_write(ssl, ptr + total, (int)(len - total));
        if (ret > 0) { total += (size_t)ret; continue; }

        int err = SSL_get_error(ssl, ret);
        if (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE) return 0;

        struct pollfd pfd = {
            .fd     = SSL_get_fd(ssl),
            .events = (short)(err == SSL_ERROR_WANT_READ ? POLLIN : POLLOUT),
        };
        int ready = poll(&pfd, 1, timeout_ms);
        if (ready < 0 && errno == EINTR) continue;
        if (ready <= 0) return 0;
    }
    return 1;
}

int tls_recv_exact(SSL* ssl, void* buf, size_t len, int timeout_ms) {
    if (!ssl) return 0;
    if (timeout_ms <= 0) timeout_ms = 5000;

    unsigned char* ptr = buf;
    size_t total = 0;
    while (total < len) {
        /* Only poll when OpenSSL has nothing buffered. One TLS record can carry
         * several protocol packets, and the descriptor goes quiet once it has
         * been read -- so polling first would wait out the timeout on bytes
         * that already arrived. */
        if (!SSL_pending(ssl)) {
            struct pollfd pfd = {.fd = SSL_get_fd(ssl), .events = POLLIN};
            int ready = poll(&pfd, 1, timeout_ms);
            if (ready < 0 && errno == EINTR) continue;
            if (ready <= 0) return 0;
        }

        int ret = SSL_read(ssl, ptr + total, (int)(len - total));
        if (ret > 0) { total += (size_t)ret; continue; }

        int err = SSL_get_error(ssl, ret);
        if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) continue;
        return 0;
    }
    return 1;
}

int tls_pending(SSL* ssl) {
    return ssl ? SSL_pending(ssl) : 0;
}

void tls_close(SSL* ssl) {
    if (!ssl) return;
    SSL_shutdown(ssl);
    if (ssl == g_tls_ssl) g_tls_ssl = NULL;
    SSL_free(ssl);
}
