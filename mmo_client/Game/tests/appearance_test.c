/**
 * @file
 * Pin the appearance word's layout and what the server will accept.
 *
 * The word is wire format shared with the server's copy of protocol.h, so the
 * bit positions here are the contract: a change that moves one is a protocol
 * bump, and this is the test that notices.
 */

#include "protocol.h"

#include <stdio.h>

static int g_failures = 0;

#define CHECK(cond, what)                                                   \
	do {                                                                    \
		if(cond) {                                                          \
			printf("  ok   %s\n", (what));                                  \
		} else {                                                            \
			printf("  FAIL %s  (%s:%d)\n", (what), __FILE__, __LINE__);     \
			g_failures++;                                                   \
		}                                                                   \
	} while(0)

int main(void) {
	printf("TEST 1: the layout is the one the spec names\n");
	CHECK(appearance_pack(APPEARANCE_BODY_MALE) == 0x0u, "zero is the male body");
	CHECK(appearance_pack(APPEARANCE_BODY_FEMALE) == 0x1u, "the body is bits 0-1");

	printf("\nTEST 2: every body survives a round trip\n");
	{
		int lost = 0;

		for(unsigned body = 0; body < APPEARANCE_BODY_COUNT; body++)
			if(appearance_body(appearance_pack(body)) != body)
				lost++;
		CHECK(lost == 0, "pack then unpack returns what went in");
	}

	printf("\nTEST 3: an out-of-range body cannot spill into the reserved bits\n");
	CHECK(appearance_pack(7) <= APPEARANCE_USED_BITS, "a wide body is cut to two bits");

	printf("\nTEST 4: validation, as the realm server applies it\n");
	CHECK(appearance_valid(0), "the zero word is valid");
	CHECK(appearance_valid(appearance_pack(APPEARANCE_BODY_FEMALE)), "so is the female body");
	CHECK(!appearance_valid(appearance_pack(2)), "a third body is refused for now");
	CHECK(!appearance_valid(appearance_pack(3)), "so is a fourth");
	CHECK(!appearance_valid(1u << 2), "a reserved bit is refused");
	CHECK(!appearance_valid(1u << 6), "so is a higher one");
	CHECK(!appearance_valid(0x80000000u), "so is the top bit");
	CHECK(!appearance_valid(0xFFFFFFFFu), "and all of them");

	printf("\n%s: %d failure(s)\n", g_failures ? "FAILED" : "PASSED", g_failures);
	return g_failures ? 1 : 0;
}
