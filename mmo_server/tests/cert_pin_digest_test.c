/**
 * @file
 * Pin one fixed certificate, against a digest produced outside this codebase.
 *
 * Every other TLS test here mints a certificate and checks that both ends agree
 * about it. That is worth having, and it is exactly the shape of test that let
 * the following bug live in the tree undetected:
 *
 *   Every pinning site called X509_pubkey_digest(), which hashes the public key
 *   BIT STRING. A pin file holds a digest of the SubjectPublicKeyInfo -- the
 *   AlgorithmIdentifier and the key bits together -- because that is what
 *   `openssl x509 -pubkey | openssl pkey -outform der | openssl dgst -sha256`
 *   produces, and what the setup script writes. The two are different bytes for
 *   the same certificate. So every pin check compared a digest of one thing
 *   against a file naming another, and refused every peer.
 *
 *   A test where the fixture and the code under test both computed it the wrong
 *   way agreed with itself perfectly.
 *
 * The certificate below is fixed, and so is the expected pin. The pin was
 * produced by the openssl command line -- the same pipeline the server's setup
 * script runs and every error message in the tree quotes -- so this test
 * compares against an authority outside the C code rather than against the C
 * code's own opinion. If cert_spki_digest() ever drifts back, this fails.
 *
 * The client tree is where the bug was live: the launcher pins the login
 * server, and with the wrong digest it would have refused every login
 * connection it ever made. This file exists in both trees and must stay
 * identical; tests/check_pinning_sync.sh enforces it.
 */

#include "cert_pin.h"
#include "cert_spki.h"

#include <openssl/pem.h>
#include <stdio.h>
#include <string.h>

static int failures = 0;

static void check(int condition, const char* what) {
    printf("  %-4s %s\n", condition ? "ok" : "FAIL", what);
    if (!condition) failures++;
}

/* A certificate generated once with:
 *   openssl req -x509 -newkey rsa:2048 -nodes -days 36500 \
 *     -subj "/CN=mmo-pin-fixture" -keyout /dev/null -out fixture.crt
 * It carries no private key here and guards nothing; it is a known input. */
static const char FIXTURE_PEM[] =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIDFzCCAf+gAwIBAgIUO4gi1ofD0v6ut1XNFYbWwzpCDIAwDQYJKoZIhvcNAQEL\n"
    "BQAwGjEYMBYGA1UEAwwPbW1vLXBpbi1maXh0dXJlMCAXDTI2MDgyNjE3NDg0MVoY\n"
    "DzIxMjYwODAyMTc0ODQxWjAaMRgwFgYDVQQDDA9tbW8tcGluLWZpeHR1cmUwggEi\n"
    "MA0GCSqGSIb3DQEBAQUAA4IBDwAwggEKAoIBAQCw/LG4quI5V7T087l1D5LmML2k\n"
    "1ZjYUZrhMYi1PMAMCpQyvydNE79hm2o3ihjq8DusIOiwuQOd+Os4LhRhDnKGaLXP\n"
    "a0jgVbDlQFR7yROUsoLUGewqBlQAUBaNzNoN3OyEjNxiX8zwXIod9ArDMz4Lverr\n"
    "fHrKoX+TLRqewOAbKZRIXoDx27pEr1nNm0MQnXftQ3YdKBx+k+sfdFwSmWY45+Hr\n"
    "n7syYSudJxTuxX/BVR89E6QlTrg/Nb+8jz956/tZNMcSQiTLOXzHNmTUY+FAX4Kb\n"
    "hZ+Twc7MqZUdipvd6pIwqHtz0lGuHcWLlafFYv0uMwBPg46rtvyHoWaoaYyBAgMB\n"
    "AAGjUzBRMB0GA1UdDgQWBBSTd5QHjndcp8uRU+ds3JBrvUnCoTAfBgNVHSMEGDAW\n"
    "gBSTd5QHjndcp8uRU+ds3JBrvUnCoTAPBgNVHRMBAf8EBTADAQH/MA0GCSqGSIb3\n"
    "DQEBCwUAA4IBAQCU6TspUXdkbnAuyVN73cdB1YXr8P8ioXCkl+PO37KjKNixycLG\n"
    "nBtDHbYTiddzMiFxj+P9dtvwzqc5KqjPPmMwZ44ydgEfh82vrhV2GxLPplXEhxlW\n"
    "a9cynyTc993QrhjaJ6avJkBZyyM1mwutz/eclSTXNOKvZDZRjNBPvZVPQwDzOoUv\n"
    "a1j1FCXG4G3WbFMTXzsl/9OoqYFXhvSYefRKpJEeljHpkKyFubwJiYQDwOBqm9FI\n"
    "MUa/PXkYJvxuEfgGBSFgJ07+a4+zFMa/fDHO3rtYIkqPDryvgp+wFnYlT457KcqN\n"
    "DYn6sISuVmxhoIWXc3fO15zTUd6aadgl9C9l\n"
    "-----END CERTIFICATE-----\n"
    ;

/* Produced by the pipeline the setup script uses:
 *   openssl x509 -in fixture.crt -pubkey -noout \
 *     | openssl pkey -pubin -outform der \
 *     | openssl dgst -sha256 -binary | openssl base64
 */
#define FIXTURE_PIN_B64 "sha256/GoRsNiZArsSUvLMj6uEghfDm6Hbwg3ybMx4dmMyhyeU="
#define FIXTURE_PIN_HEX "1a846c362640aec494bcb323eae12085f0e6e876f0837c9b331e1d98cca1c9e5"

/* What X509_pubkey_digest() returns for the same certificate. Named here so the
 * test can assert the two are different -- if a future OpenSSL made them equal,
 * the case above would pass for the wrong reason and this would say so. */
#define BITSTRING_DIGEST_HEX "4ab17691f5b0f485abae84076266c81dc0914d006e3adedb1bdc7320e859c5ef"

int main(void) {
    printf("=== public-key pin digest ===\n");

    BIO* bio = BIO_new_mem_buf(FIXTURE_PEM, -1);
    X509* cert = PEM_read_bio_X509(bio, NULL, NULL, NULL);
    BIO_free(bio);
    check(cert != NULL, "the fixture certificate parses");
    if (!cert) return 1;

    unsigned char digest[CERT_PIN_DIGEST_LEN];
    check(cert_spki_digest(cert, digest) == 1, "it fingerprints");

    char hex[CERT_PIN_DIGEST_LEN * 2 + 1];
    cert_pin_format(digest, hex, sizeof(hex));
    check(strcmp(hex, FIXTURE_PIN_HEX) == 0,
          "and the digest is the one openssl(1) computes for its SPKI");
    if (strcmp(hex, FIXTURE_PIN_HEX) != 0)
        printf("       got %s\n       want %s\n", hex, FIXTURE_PIN_HEX);

    check(strcmp(hex, BITSTRING_DIGEST_HEX) != 0,
          "and is NOT the bit-string digest X509_pubkey_digest returns");

    /* The end-to-end claim: a pin file written by the setup script names this
     * certificate, and the digest the code computes matches it. */
    CertPinSet set;
    cert_pin_reset(&set);
    check(cert_pin_add_line(&set, FIXTURE_PIN_B64) == 1,
          "the base64 pin line from a pin file parses");
    check(cert_pin_matches(&set, digest) == 1,
          "and it matches the certificate it was generated from");

    /* The negative: a pin file naming something else refuses this certificate. */
    CertPinSet other;
    cert_pin_reset(&other);
    check(cert_pin_add_line(&other, BITSTRING_DIGEST_HEX) == 1, "another pin parses");
    check(cert_pin_matches(&other, digest) == 0,
          "and a pin file naming a different key refuses this one");

    X509_free(cert);

    if (failures) { printf("\n%d FAILED\n", failures); return 1; }
    printf("\nall pin digest checks passed\n");
    return 0;
}
