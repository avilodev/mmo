/**
 * @file
 * Fingerprint a certificate's public key as SHA-256 over its DER SPKI.
 */
#include "cert_spki.h"

#include <openssl/evp.h>
#include <openssl/ssl.h>

int cert_spki_digest(X509* cert, unsigned char* out) {
    if (!cert || !out) return 0;

    /* i2d_X509_PUBKEY, not X509_pubkey_digest: the latter hashes the key bit
     * string alone and would disagree with every pin file in the tree. See
     * cert_spki.h. */
    unsigned char* der = NULL;
    int der_len = i2d_X509_PUBKEY(X509_get_X509_PUBKEY(cert), &der);
    if (der_len <= 0 || !der) return 0;

    unsigned int len = 0;
    int ok = EVP_Digest(der, (size_t)der_len, out, &len, EVP_sha256(), NULL) == 1
             && len == 32;
    OPENSSL_free(der);
    return ok;
}

int cert_spki_digest_peer(SSL* ssl, unsigned char* out) {
    if (!ssl) return 0;

    /* Renamed in OpenSSL 3.0; the 1.1 spelling still works there but is
     * deprecated, and this builds against whatever the platform ships. */
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
    X509* cert = SSL_get1_peer_certificate(ssl);
#else
    X509* cert = SSL_get_peer_certificate(ssl);
#endif
    if (!cert) return 0;

    int ok = cert_spki_digest(cert, out);
    X509_free(cert);
    return ok;
}
