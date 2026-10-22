// Check the character select cycle: the new character entry is always last.

#include "ui/char_select_nav.h"

#include <assert.h>
#include <stdio.h>

static void test_count(void) {
	assert(cs_nav_count(0) == 1);
	assert(cs_nav_count(3) == 4);
	assert(cs_nav_count(10) == 11);
	assert(cs_nav_count(-2) == 1);
}

static void test_clamp(void) {
	assert(cs_nav_clamp(-1, 3) == 0);
	assert(cs_nav_clamp(0, 3) == 0);
	assert(cs_nav_clamp(2, 3) == 2);
	assert(cs_nav_clamp(3, 3) == 3);
	assert(cs_nav_clamp(4, 3) == 3);
	assert(cs_nav_clamp(7, 0) == 0);
}

static void test_step(void) {
	// forward through three characters, onto new character, and round
	assert(cs_nav_step(0, 3, 1) == 1);
	assert(cs_nav_step(2, 3, 1) == 3);
	assert(cs_nav_step(3, 3, 1) == 0);

	// backward from the first lands on new character
	assert(cs_nav_step(0, 3, -1) == 3);
	assert(cs_nav_step(3, 3, -1) == 2);

	// no list: the one entry stays put
	assert(cs_nav_step(0, 0, 1) == 0);
	assert(cs_nav_step(0, 0, -1) == 0);

	// no direction, or a stale index, still ends in range
	assert(cs_nav_step(2, 3, 0) == 2);
	assert(cs_nav_step(-1, 3, 1) == 1);
	assert(cs_nav_step(9, 3, 1) == 0);
}

static void test_is_new(void) {
	assert(!cs_nav_is_new(0, 3));
	assert(!cs_nav_is_new(2, 3));
	assert(cs_nav_is_new(3, 3));
	assert(cs_nav_is_new(0, 0));
	assert(cs_nav_is_new(-1, 0));
	assert(!cs_nav_is_new(-1, 2));
}

int main(void) {
	test_count();
	test_clamp();
	test_step();
	test_is_new();
	printf("char_select_nav_test: ok\n");

	return 0;
}
