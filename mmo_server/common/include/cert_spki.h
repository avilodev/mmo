#ifndef CERT_SPKI_H
#define CERT_SPKI_H

/** @file Fingerprint a certificate's public key the way a pin file names it.
 *
 * Separate from cert_pin.h, which is deliberately free of OpenSSL so it can be
 * built and tested on the host. This is the one piece that cannot be: turning
 * an X509 into the 32 bytes cert_pin_matches() compares.
 *
 * What is hashed is the DER SubjectPublicKeyInfo -- the AlgorithmIdentifier and
 * the key bits together -- because that is what every tool that produces a pin
 * produces, including the openssl pipeline the setup script uses and the one
 * quoted in every error message here:
 *
 *   openssl x509 -in server.crt -pubkey -noout \
 *     | openssl pkey -pubin -outform der \
 *     | openssl dgst -sha256 -binary | openssl base64
 *
 * It is emphatically NOT what X509_pubkey_digest() returns. That function
 * hashes the public key BIT STRING alone, without the AlgorithmIdentifier
 * around it, so it produces a different digest for the same certificate. Every
 * pinning site in both trees called it, which meant every pin check compared a
 * digest of one thing against a pin file naming another and refused every
 * peer -- consistently, and therefore invisibly, because the two ends of a test
 * that both computed it the wrong way still agreed with each other.
 *
 * This file exists in both trees and must stay byte-identical; the server's
 * tests/check_single_definition.sh enforces it.
 */

#include <openssl/x509.h>

/** SHA-256 over the DER SubjectPublicKeyInfo of a certificate's public key.
 *
 * @param out  Receives 32 bytes.
 * @return 1 on success, 0 when the certificate has no encodable public key.
 */
int cert_spki_digest(X509* cert, unsigned char* out);

/** The same, for the peer of a completed handshake.
 *
 * @return 1 on success, 0 when the peer presented nothing usable.
 */
int cert_spki_digest_peer(SSL* ssl, unsigned char* out);

#endif // CERT_SPKI_H
