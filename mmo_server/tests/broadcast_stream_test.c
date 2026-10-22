/**
 * @file
 * Check when a nearby-entities list is sent, and the speed other clients animate by.
 */

#include "anim_speed.h"
#include "list_gate.h"
#include "move_validator.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>

static list_gate g_gate;

// A list with entries is always sent
static void test_entries_always_go(void) {
	printf("  a list with entries is always sent\n");
	assert(list_gate_due(&g_gate, 5, 3));
	assert(list_gate_due(&g_gate, 5, 3));
	assert(list_gate_due(&g_gate, 5, 1));
}

// The last neighbour leaving is told once, then the client is left alone
static void test_empty_list_goes_once(void) {
	printf("  an empty list goes once after a full one, then not again\n");
	assert(list_gate_due(&g_gate, 6, 2));
	assert(list_gate_due(&g_gate, 6, 0));
	assert(!list_gate_due(&g_gate, 6, 0));
	assert(!list_gate_due(&g_gate, 6, 0));
	assert(list_gate_due(&g_gate, 6, 1));
	assert(list_gate_due(&g_gate, 6, 0));
}

// A fresh connection is told its list even when it is empty, so it knows it has arrived
static void test_first_list_always_goes(void) {
	printf("  a fresh connection is sent its first list, empty or not, then left alone\n");
	assert(list_gate_due(&g_gate, 7, 0));
	assert(!list_gate_due(&g_gate, 7, 0));
	assert(!list_gate_due(&g_gate, 7, 0));
	assert(list_gate_due(&g_gate, 7, 2));
	assert(list_gate_due(&g_gate, 7, 0));
	assert(!list_gate_due(&g_gate, 7, 0));
}

// A new connection on a reused descriptor starts fresh
static void test_reset_makes_the_next_list_the_first(void) {
	printf("  a reset descriptor is sent its first list again\n");
	assert(list_gate_due(&g_gate, 10, 0));
	assert(!list_gate_due(&g_gate, 10, 0));
	list_gate_reset(&g_gate, 10);
	assert(list_gate_due(&g_gate, 10, 0));
	assert(!list_gate_due(&g_gate, 10, 0));

	assert(list_gate_due(&g_gate, 11, 3));
	list_gate_reset(&g_gate, 11);
	assert(list_gate_due(&g_gate, 11, 0));
	list_gate_reset(&g_gate, -1);
	list_gate_reset(&g_gate, LIST_GATE_FDS);
}

// One descriptor's history is not another's
static void test_descriptors_are_independent(void) {
	printf("  each descriptor has its own history\n");
	assert(list_gate_due(&g_gate, 8, 4));
	assert(list_gate_due(&g_gate, 9, 0));
	assert(!list_gate_due(&g_gate, 9, 0));
	assert(list_gate_due(&g_gate, 8, 0));
}

// A descriptor outside the table still gets its populated lists
static void test_out_of_range_descriptor(void) {
	printf("  a descriptor outside the table is not gated\n");
	assert(list_gate_due(&g_gate, LIST_GATE_FDS, 2));
	assert(!list_gate_due(&g_gate, LIST_GATE_FDS, 0));
	assert(!list_gate_due(&g_gate, -1, 0));
}

static void test_speed_is_the_velocity_length(void) {
	printf("  the reported speed is the length of the velocity\n");
	assert(fabsf(anim_speed_reported(30.0f, 40.0f, 200.0f) - 50.0f) < 1e-3f);
	assert(anim_speed_reported(0.0f, 0.0f, 200.0f) == 0.0f);
}

// A client cannot make its body look faster than the server allows it to be
static void test_speed_is_capped(void) {
	float ceiling = 200.0f * MOVE_SPEED_TOLERANCE;

	printf("  the reported speed cannot exceed what the server allows\n");
	assert(anim_speed_reported(9000.0f, 0.0f, 200.0f) == ceiling);
	assert(anim_speed_reported(9000.0f, 0.0f, 0.0f) == MOVE_DEFAULT_SPEED * MOVE_SPEED_TOLERANCE);
	assert(anim_speed_reported(9000.0f, 0.0f, NAN) == MOVE_DEFAULT_SPEED * MOVE_SPEED_TOLERANCE);
}

static void test_bad_velocity_is_standing(void) {
	printf("  a velocity that is not a number reads as standing\n");
	assert(anim_speed_reported(NAN, 5.0f, 200.0f) == 0.0f);
	assert(anim_speed_reported(INFINITY, 0.0f, 200.0f) == 0.0f);
}

static void test_speed_goes_stale(void) {
	printf("  a speed with no move behind it for a while is zero\n");
	assert(anim_speed_current(150.0f, 10.0, 10.1) == 150.0f);
	assert(anim_speed_current(150.0f, 10.0, 10.0 + ANIM_SPEED_STALE_SECONDS + 0.01) == 0.0f);
}

int main(void) {
	printf("=== broadcast stream ===\n");
	test_entries_always_go();
	test_empty_list_goes_once();
	test_first_list_always_goes();
	test_reset_makes_the_next_list_the_first();
	test_descriptors_are_independent();
	test_out_of_range_descriptor();
	test_speed_is_the_velocity_length();
	test_speed_is_capped();
	test_bad_velocity_is_standing();
	test_speed_goes_stale();
	printf("=== broadcast stream: all checks passed ===\n");

	return 0;
}
