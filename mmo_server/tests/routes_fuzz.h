#ifndef ROUTES_FUZZ_H
#define ROUTES_FUZZ_H

/**
 * @file
 * A shared corpus and scoreboard for fuzzing the three server packet routers.
 *
 * The client's four dispatchers have had a fuzzer since the P1 pass. The
 * server's three did not, which is the wrong way round: the client parses
 * bytes chosen by a server its user picked, and the server parses bytes chosen
 * by anyone who can open a socket to it.
 *
 * What is being looked for is a router that hands a handler a buffer shorter
 * than the packet that handler is written against. Every router is a switch on
 * one byte the sender chose, and the length check for each case is written by
 * hand next to it -- so the failure is one missing `if (bytes >= sizeof(...))`,
 * which is invisible in review and silent at runtime until the read lands
 * somewhere interesting.
 *
 * Two things make that detectable here:
 *
 *   1. Each router's test declares, per opcode, the smallest buffer its
 *      handler may be entered with. The stubs record what they were actually
 *      given, and any call below the declared floor is a failure. That table
 *      is the specification -- it is written from protocol.h, not from the
 *      router, so a router that stops checking does not quietly take the
 *      table with it.
 *
 *   2. Every fixture is a heap allocation of exactly the length being passed,
 *      never a big static buffer with a small length. A router that reads one
 *      byte past what it was given is then a heap overflow rather than a
 *      read of whatever the previous fixture left behind, which is what makes
 *      the sanitizer build (`make routes-fuzz-sanitize`) worth running.
 */

#include "protocol.h"

#include <arpa/inet.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- Scoreboard ---------------------------------------------------------- */

static int fz_failures = 0;

#define FZ_CHECK(cond, what)                                                \
    do {                                                                    \
        if (cond) {                                                         \
            printf("  ok   %s\n", (what));                                  \
        } else {                                                            \
            printf("  FAIL %s  (%s:%d)\n", (what), __FILE__, __LINE__);     \
            fz_failures++;                                                  \
        }                                                                   \
    } while (0)

/* --- What the router promises each handler ------------------------------- */

/** The smallest buffer, in bytes, that opcode's handler may be entered with. */
static uint16_t fz_floor[256];

/** Opcodes seen entering a handler at all, so a silent router is visible too. */
static int fz_reached[256];

/** Violations, recorded rather than aborted on, so one run reports all of them. */
static int fz_violations = 0;

/** Declare the floor for one opcode. */
static void fz_declare(uint8_t opcode, size_t min_bytes) {
    fz_floor[opcode] = (uint16_t)min_bytes;
}

/**
 * Record that a handler was entered, and check it against the declared floor.
 *
 * Called from every stub. `name` is only for the failure message.
 */
static void fz_entered(const char* name, uint8_t opcode, size_t bytes) {
    fz_reached[opcode] = 1;
    if (bytes < fz_floor[opcode]) {
        if (fz_violations < 20)
            printf("  FAIL %s entered for opcode %u with %zu bytes; "
                   "the router promises at least %u\n",
                   name, opcode, bytes, fz_floor[opcode]);
        fz_violations++;
    }
}

/* --- The corpus ---------------------------------------------------------- */

/** A deterministic PRNG, so a failure is reproducible from the seed alone. */
static uint32_t fz_rng_state = 0x5eed1234u;

static uint32_t fz_rand(void) {
    fz_rng_state ^= fz_rng_state << 13;
    fz_rng_state ^= fz_rng_state >> 17;
    fz_rng_state ^= fz_rng_state << 5;
    return fz_rng_state;
}

/**
 * Build one fixture in a heap block of exactly `len` bytes.
 *
 * Exactly `len`, never a static buffer with a shorter declared length: that is
 * what turns a one-byte overread into something a sanitizer can see.
 *
 * @param opcode  Written into header.type when the buffer is long enough.
 * @param fill    0 to zero the payload, 1 to fill it with pseudo-random bytes.
 * @return        The block, which the caller frees.
 */
static uint8_t* fz_fixture(uint8_t opcode, size_t len, int fill) {
    uint8_t* buf = malloc(len ? len : 1);
    if (!buf) { perror("malloc"); exit(1); }

    for (size_t i = 0; i < len; i++)
        buf[i] = fill ? (uint8_t)(fz_rand() >> 16) : 0;

    if (len >= 1) buf[0] = opcode;

    /* A declared payload_size that agrees with the buffer, when there is room
     * to write one. A router that trusts the declaration instead of the length
     * it was handed is a separate bug, and the random pass below leaves the
     * field arbitrary so that case is covered too. */
    if (!fill && len >= sizeof(PacketHeader)) {
        PacketHeader* h = (PacketHeader*)buf;
        h->payload_size = htons((uint16_t)(len - sizeof(PacketHeader)));
    }

    return buf;
}

#endif // ROUTES_FUZZ_H
