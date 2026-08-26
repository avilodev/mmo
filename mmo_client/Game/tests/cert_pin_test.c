/**
 * @file
 * Exercise the launcher's public-key pin parsing and matching.
 *
 * The pin set is what decides whether a TLS peer is the login server, so the
 * cases that matter are the ones where a malformed or near-miss pin might be
 * accepted: the whole point of the file is to fail closed.
 */
#include "cert_pin.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures = 0;

#define CHECK(cond, what)                                                   \
    do {                                                                    \
        if (cond) {                                                         \
            printf("  ok   %s\n", (what));                                  \
        } else {                                                            \
            printf("  FAIL %s  (%s:%d)\n", (what), __FILE__, __LINE__);     \
            g_failures++;                                                   \
        }                                                                   \
    } while (0)

/* The development login certificate's SPKI digest, as produced by
 * Launcher/certs/make_pin.sh. */
static const char* DEV_PIN_B64 = "sha256/WMNNAStws+GKVUd05oEbdY2LMvFf8E+GQYhGFOARikQ=";

/** Decode the same digest a second way, so the two decoders can be compared. */
static const char* DEV_PIN_HEX =
    "58c34d012b70b3e18a554774e6811b758d8b32f15ff04f8641884614e0118a44";

int main(void) {
    CertPinSet set;

    printf("TEST 1: an empty set accepts nothing\n");
    cert_pin_reset(&set);
    {
        uint8_t anything[CERT_PIN_DIGEST_LEN] = {0};
        CHECK(cert_pin_is_empty(&set), "a reset set reports empty");
        CHECK(!cert_pin_matches(&set, anything), "an empty set matches no digest");
    }

    printf("\nTEST 2: base64 and hex spellings decode to the same digest\n");
    {
        CertPinSet from_b64, from_hex;
        cert_pin_reset(&from_b64);
        cert_pin_reset(&from_hex);

        CHECK(cert_pin_add_line(&from_b64, DEV_PIN_B64), "the base64 form parses");
        CHECK(cert_pin_add_line(&from_hex, DEV_PIN_HEX), "the hex form parses");
        CHECK(from_b64.count == 1 && from_hex.count == 1, "each stored exactly one pin");
        CHECK(memcmp(from_b64.digests[0], from_hex.digests[0],
                     CERT_PIN_DIGEST_LEN) == 0,
              "both spellings produce the same 32 bytes");

        CHECK(cert_pin_matches(&from_b64, from_hex.digests[0]),
              "a pinned digest matches");
    }

    printf("\nTEST 3: a single wrong bit is refused\n");
    {
        cert_pin_reset(&set);
        cert_pin_add_line(&set, DEV_PIN_B64);

        uint8_t near_miss[CERT_PIN_DIGEST_LEN];
        memcpy(near_miss, set.digests[0], sizeof(near_miss));
        near_miss[CERT_PIN_DIGEST_LEN - 1] ^= 0x01;
        CHECK(!cert_pin_matches(&set, near_miss), "a last-byte flip does not match");

        memcpy(near_miss, set.digests[0], sizeof(near_miss));
        near_miss[0] ^= 0x80;
        CHECK(!cert_pin_matches(&set, near_miss), "a first-byte flip does not match");
    }

    printf("\nTEST 4: comments and blank lines are skipped, not stored\n");
    {
        cert_pin_reset(&set);
        CHECK(cert_pin_add_line(&set, "# a comment"), "a comment is accepted");
        CHECK(cert_pin_add_line(&set, "   "), "whitespace is accepted");
        CHECK(cert_pin_add_line(&set, ""), "an empty line is accepted");
        CHECK(cert_pin_is_empty(&set), "none of them added a pin");
    }

    printf("\nTEST 5: malformed pins are refused rather than truncated\n");
    {
        cert_pin_reset(&set);
        CHECK(!cert_pin_add_line(&set, "sha256/short"), "a short base64 pin fails");
        CHECK(!cert_pin_add_line(&set, "sha256/WMNNAStws+GKVUd05oEbdY2LMvFf8E+GQYhGFOARik="),
              "a base64 pin one character short fails");
        CHECK(!cert_pin_add_line(&set, "sha256/WMNNAStws+GKVUd05oEbdY2LMvFf8E+GQYhGFOARik*="),
              "a base64 pin with an illegal character fails");
        CHECK(!cert_pin_add_line(&set, "58c34d012b70b3e18a554774e6811b75"),
              "a 16-byte hex digest fails");
        CHECK(!cert_pin_add_line(&set, "zzzz"), "arbitrary text fails");
        CHECK(cert_pin_is_empty(&set), "no malformed line was stored");
    }

    printf("\nTEST 6: colon-separated hex, as openssl prints it, is accepted\n");
    {
        cert_pin_reset(&set);
        char colonised[CERT_PIN_DIGEST_LEN * 3];
        size_t out = 0;
        for (int i = 0; i < CERT_PIN_DIGEST_LEN; i++) {
            if (i) colonised[out++] = ':';
            colonised[out++] = DEV_PIN_HEX[i * 2];
            colonised[out++] = DEV_PIN_HEX[i * 2 + 1];
        }
        colonised[out] = '\0';

        CHECK(cert_pin_add_line(&set, colonised), "colon-separated hex parses");

        CertPinSet plain;
        cert_pin_reset(&plain);
        cert_pin_add_line(&plain, DEV_PIN_HEX);
        CHECK(set.count == 1 &&
              memcmp(set.digests[0], plain.digests[0], CERT_PIN_DIGEST_LEN) == 0,
              "it decodes to the same digest as the unseparated form");
    }

    printf("\nTEST 7: the set is bounded and refuses overflow\n");
    {
        cert_pin_reset(&set);
        for (int i = 0; i < CERT_PIN_MAX; i++)
            CHECK(cert_pin_add_line(&set, DEV_PIN_B64), "a pin fits while there is room");
        CHECK(set.count == CERT_PIN_MAX, "the set filled to its capacity");
        CHECK(!cert_pin_add_line(&set, DEV_PIN_B64), "one pin past capacity is refused");
        CHECK(set.count == CERT_PIN_MAX, "the refusal did not grow the set");
    }

    printf("\nTEST 8: loading a file with a bad line yields no pins at all\n");
    {
        const char* good_path = "/tmp/cert_pin_test_good.txt";
        const char* bad_path  = "/tmp/cert_pin_test_bad.txt";
        char reason[256];

        FILE* f = fopen(good_path, "w");
        if (!f) { printf("  FAIL cannot write the fixture\n"); return 1; }
        fprintf(f, "# leading comment\n\n%s\n", DEV_PIN_B64);
        fclose(f);

        cert_pin_reset(&set);
        CHECK(cert_pin_load_file(&set, good_path, reason, sizeof(reason)),
              "a well-formed file loads");
        CHECK(set.count == 1, "it yielded exactly one pin");

        f = fopen(bad_path, "w");
        if (!f) { printf("  FAIL cannot write the fixture\n"); return 1; }
        fprintf(f, "%s\nnot-a-pin\n", DEV_PIN_B64);
        fclose(f);

        cert_pin_reset(&set);
        CHECK(!cert_pin_load_file(&set, bad_path, reason, sizeof(reason)),
              "one bad line fails the whole file");
        CHECK(cert_pin_is_empty(&set),
              "the good pin before it is discarded too, so the set is never "
              "smaller than the operator intended without saying so");
        CHECK(reason[0] != '\0', "the failure names the offending line");

        cert_pin_reset(&set);
        CHECK(!cert_pin_load_file(&set, "/tmp/cert_pin_test_absent.txt",
                                  reason, sizeof(reason)),
              "a missing file fails");
        CHECK(reason[0] != '\0', "the failure says the file could not be opened");

        remove(good_path);
        remove(bad_path);
    }

    printf("\nTEST 9: an empty file is a failure, not an empty allowlist\n");
    {
        const char* path = "/tmp/cert_pin_test_empty.txt";
        char reason[256];

        FILE* f = fopen(path, "w");
        if (!f) { printf("  FAIL cannot write the fixture\n"); return 1; }
        fprintf(f, "# nothing but comments\n");
        fclose(f);

        cert_pin_reset(&set);
        CHECK(!cert_pin_load_file(&set, path, reason, sizeof(reason)),
              "a file with no pins fails to load");
        remove(path);
    }

    printf("\nTEST 10: digests render as 64 lowercase hex characters\n");
    {
        cert_pin_reset(&set);
        cert_pin_add_line(&set, DEV_PIN_B64);

        char text[CERT_PIN_DIGEST_LEN * 2 + 1];
        cert_pin_format(set.digests[0], text, sizeof(text));
        CHECK(strcmp(text, DEV_PIN_HEX) == 0, "the rendering round-trips the hex form");

        char tiny[8];
        cert_pin_format(set.digests[0], tiny, sizeof(tiny));
        CHECK(tiny[0] == '\0', "a buffer too small produces an empty string");
    }

    printf("\n");
    if (g_failures) {
        printf("%d ASSERTION%s FAILED\n", g_failures, g_failures == 1 ? "" : "S");
        return 1;
    }
    printf("ALL ASSERTIONS PASSED\n");
    return 0;
}
