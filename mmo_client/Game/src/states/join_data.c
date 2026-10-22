// Remember which join packets have arrived, and when the quest list is whole.

#include "states/join_data.h"

static volatile unsigned g_seen;
static unsigned g_expected;
static unsigned g_received;
static int g_list_started;

void join_data_reset(void) {
	g_seen = 0;
	g_expected = 0;
	g_received = 0;
	g_list_started = 0;
}

void join_data_note(unsigned flags) {
	g_seen |= flags;
}

int join_data_seen(unsigned flags) {
	return (g_seen & flags) == flags;
}

void join_data_available_reset(unsigned expected) {
	g_expected = expected;
	g_received = 0;
	g_list_started = 1;
	if(expected == 0)
		join_data_note(JOIN_QUESTS);
}

void join_data_available_entry(void) {
	if(!g_list_started)
		return;
	g_received++;
	if(g_received >= g_expected)
		join_data_note(JOIN_QUESTS);
}
