// Check the record of which join packets have arrived.
#include "states/join_data.h"
#include <assert.h>
#include <stdio.h>

static void test_nothing_seen_after_reset(void) {
	join_data_note(JOIN_STATS);
	join_data_reset();
	assert(!join_data_seen(JOIN_STATS));
	assert(!join_data_seen(JOIN_ABILITIES));
}

static void test_seen_needs_every_flag_asked_for(void) {
	join_data_reset();
	join_data_note(JOIN_ABILITIES_HUMAN);
	assert(join_data_seen(JOIN_ABILITIES_HUMAN));
	assert(!join_data_seen(JOIN_ABILITIES));
	join_data_note(JOIN_ABILITIES_ANIMAL);
	assert(join_data_seen(JOIN_ABILITIES));
}

static void test_flags_are_independent(void) {
	join_data_reset();
	join_data_note(JOIN_FRIENDS);
	assert(join_data_seen(JOIN_FRIENDS));
	assert(!join_data_seen(JOIN_FRIEND_REQUESTS));
	assert(!join_data_seen(JOIN_ONLINE_LIST));
	assert(!join_data_seen(JOIN_QUESTS));
}

static void test_available_quests_done_when_all_entries_arrive(void) {
	join_data_reset();
	join_data_available_reset(3);
	assert(!join_data_seen(JOIN_QUESTS));
	join_data_available_entry();
	join_data_available_entry();
	assert(!join_data_seen(JOIN_QUESTS));
	join_data_available_entry();
	assert(join_data_seen(JOIN_QUESTS));
}

static void test_empty_available_list_is_done_at_once(void) {
	join_data_reset();
	join_data_available_reset(0);
	assert(join_data_seen(JOIN_QUESTS));
}

static void test_entry_before_its_reset_does_not_count(void) {
	join_data_reset();
	join_data_available_entry();
	join_data_available_reset(2);
	assert(!join_data_seen(JOIN_QUESTS));
	join_data_available_entry();
	join_data_available_entry();
	assert(join_data_seen(JOIN_QUESTS));
}

int main(void) {
	test_nothing_seen_after_reset();
	test_seen_needs_every_flag_asked_for();
	test_flags_are_independent();
	test_available_quests_done_when_all_entries_arrive();
	test_empty_available_list_is_done_at_once();
	test_entry_before_its_reset_does_not_count();
	printf("join_data_test: all tests passed\n");
	return 0;
}
