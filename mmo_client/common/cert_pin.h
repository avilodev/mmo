#ifndef CERT_PIN_H
#define CERT_PIN_H

/** @file Hold the set of public-key fingerprints a peer will accept.
 *
 * Nothing in this system has a certificate issued by a public CA, so chain
 * verification alone cannot decide whether a peer is who it claims to be. What
 * can decide it is a fingerprint of that peer's public key, provisioned ahead
 * of time and compared after every handshake.
 *
 * Four links use this, and they are all the same problem seen from different
 * ends: the launcher pins the login server, the game pins the realm, the realm
 * pins each world, and each world pins the realm back.
 *
 * Deliberately free of OpenSSL and Windows: parsing a pin file and comparing
 * digests is ordinary logic, and keeping it separate is what lets it be built
 * and tested on the host rather than only inside a Windows launcher build.
 *
 * This file exists in both trees and must stay byte-identical; tests/
 * check_single_definition.sh enforces it.
 */

#include <stddef.h>
#include <stdint.h>

/** SHA-256 produces 32 bytes. */
#define CERT_PIN_DIGEST_LEN 32

/** How many pins one launcher may carry.
 *
 * More than one so a certificate can be rotated: ship the next key's pin
 * alongside the current one, roll the server, then drop the old pin.
 */
#define CERT_PIN_MAX 8

/** A parsed set of accepted public-key fingerprints. */
typedef struct {
    uint8_t digests[CERT_PIN_MAX][CERT_PIN_DIGEST_LEN];
    int     count;
} CertPinSet;

/** Empty the set. An empty set accepts nothing. */
void cert_pin_reset(CertPinSet* set);

/** Report whether the set holds no pins. */
int cert_pin_is_empty(const CertPinSet* set);

/**
 * Parse and append one pin line.
 *
 * Accepted forms, matching what `openssl` prints for an SPKI digest:
 *   - `sha256/<base64>`   (44 base64 characters, HPKP style)
 *   - `<base64>`          (the same without the prefix)
 *   - `<64 hex digits>`   (with or without `:` separators)
 *
 * Blank lines and lines beginning with `#` are ignored and reported as success,
 * so a whole file can be fed through this function line by line.
 *
 * @return 1 when the line was a comment, blank, or a pin that was stored;
 *         0 when the line was malformed or the set is full.
 */
int cert_pin_add_line(CertPinSet* set, const char* line);

/**
 * Load a pin file.
 *
 * Fails on the first malformed line rather than skipping it: a pin file the
 * operator got wrong must not silently produce a smaller set than intended.
 *
 * @param error       Receives a human-readable reason on failure; may be NULL.
 * @param error_size  Size of that buffer.
 * @return            1 when the file was read and yielded at least one pin.
 */
int cert_pin_load_file(CertPinSet* set, const char* path,
                       char* error, size_t error_size);

/**
 * Test one 32-byte digest against the set.
 *
 * The comparison runs over every pin without an early exit. A pin is not a
 * secret, but the habit is cheap and this is the function an attacker gets to
 * call repeatedly.
 *
 * @return 1 when some pin matches, otherwise 0.
 */
int cert_pin_matches(const CertPinSet* set, const uint8_t* digest);

/**
 * Render a digest as lowercase hex for a log line.
 *
 * @param out       Buffer receiving 64 characters plus a terminator.
 * @param out_size  Size of that buffer; must be at least 65.
 */
void cert_pin_format(const uint8_t* digest, char* out, size_t out_size);

#endif // CERT_PIN_H
