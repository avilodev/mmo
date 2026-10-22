// Check which status the server shows for a player.

#include "status_rules.h"

#include <assert.h>
#include <stdio.h>

static void test_online(void) {
	assert(status_effective(SESSION_STATUS_ONLINE, 1000.0, 1000.0) == SESSION_STATUS_ONLINE);
	assert(status_effective(SESSION_STATUS_ONLINE, 1000.0, 1000.0 + AFK_IDLE_SECONDS - 1.0)
	       == SESSION_STATUS_ONLINE);
}

static void test_idle_online_is_afk(void) {
	// the line itself counts as idle
	assert(status_effective(SESSION_STATUS_ONLINE, 1000.0, 1000.0 + AFK_IDLE_SECONDS)
	       == SESSION_STATUS_AFK);
	assert(status_effective(SESSION_STATUS_ONLINE, 1000.0, 1000.0 + AFK_IDLE_SECONDS * 10.0)
	       == SESSION_STATUS_AFK);
}

static void test_chosen_status_wins(void) {
	// a chosen status shows however recently they acted
	assert(status_effective(SESSION_STATUS_AFK, 1000.0, 1000.0) == SESSION_STATUS_AFK);
	assert(status_effective(SESSION_STATUS_DND, 1000.0, 1000.0) == SESSION_STATUS_DND);
	assert(status_effective(SESSION_STATUS_OFFLINE, 1000.0, 1000.0) == SESSION_STATUS_OFFLINE);
}

static void test_idle_never_overrides_a_choice(void) {
	double later = 1000.0 + AFK_IDLE_SECONDS * 10.0;

	assert(status_effective(SESSION_STATUS_DND, 1000.0, later) == SESSION_STATUS_DND);
	assert(status_effective(SESSION_STATUS_OFFLINE, 1000.0, later) == SESSION_STATUS_OFFLINE);
}

static void test_clock_behind(void) {
	// a stamp from the future never reads as idle
	assert(status_effective(SESSION_STATUS_ONLINE, 2000.0, 1000.0) == SESSION_STATUS_ONLINE);
}

static void test_unknown_choice_is_online(void) {
	// a stored value outside the list falls back to the idle rule
	assert(status_effective(200, 1000.0, 1000.0) == SESSION_STATUS_ONLINE);
	assert(status_effective(200, 1000.0, 1000.0 + AFK_IDLE_SECONDS) == SESSION_STATUS_AFK);
}

int main(void) {
	test_online();
	test_idle_online_is_afk();
	test_chosen_status_wins();
	test_idle_never_overrides_a_choice();
	test_clock_behind();
	test_unknown_choice_is_online();
	printf("status_rules_test: OK\n");

	return 0;
}
