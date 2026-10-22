/**
 * @file
 * The quests the server says the character has already finished.
 *
 * A finished quest is not kept by the client between sessions, so what the
 * server names on entry is the whole of the completed list. It must not cost an
 * in-progress quest its place, and must not turn a quest under way into a
 * finished one.
 */

#include "ui/quest_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int passed = 0;

static void check(int condition, const char* what) {
	if(!condition) {
		printf("  FAIL: %s\n", what);
		exit(1);
	}
	passed++;
}

static void fresh_log(QuestLogState* ql) {
	memset(ql, 0, sizeof(*ql));
	ql->selected = -1;
}

// Hold a quest in progress the way quest_log_add does; that lives with the drawing code.
static void hold_quest(QuestLogState* ql, uint32_t id) {
	QuestEntry* e = &ql->entries[ql->count++];

	memset(e, 0, sizeof(*e));
	e->id = id;
	e->active = 1;
}

static void test_a_finished_quest_is_listed(void) {
	QuestLogState ql;

	printf("a finished quest is listed as completed\n");
	fresh_log(&ql);
	quest_log_finished_add(&ql, 301, "A Coin to Start");

	check(ql.count == 1, "one entry");
	check(ql.entries[0].id == 301, "the id is kept");
	check(strcmp(ql.entries[0].title, "A Coin to Start") == 0, "the title is kept");
	check(ql.entries[0].completed == 1 && ql.entries[0].active == 0, "completed, not active");
}

static void test_a_quest_under_way_stays_in_progress(void) {
	QuestLogState ql;

	printf("a repeatable under way is not called finished\n");
	fresh_log(&ql);
	hold_quest(&ql, 301);
	quest_log_finished_add(&ql, 301, "A Coin to Start");

	check(ql.count == 1, "no second entry");
	check(ql.entries[0].active == 1 && ql.entries[0].completed == 0, "still in progress");

	quest_log_finished_add(&ql, 302, "Next");
	quest_log_finished_add(&ql, 302, "Next");
	check(ql.count == 2, "the same finished quest is listed once");
}

static void test_finished_quests_do_not_crowd_out_new_ones(void) {
	QuestLogState ql;

	printf("a full log of finished quests still takes a new quest\n");
	fresh_log(&ql);
	for(uint32_t id = 1; id <= MAX_QUESTS + 5; id++)
		quest_log_finished_add(&ql, id, "Old");
	check(ql.count == MAX_QUESTS, "the list stops at the limit");

	check(quest_log_make_room(&ql), "a finished quest makes room");
	check(ql.count == MAX_QUESTS - 1, "one entry fewer");
	check(ql.entries[0].id == 2, "the oldest finished quest made way");
}

static void test_quests_in_progress_are_never_dropped(void) {
	QuestLogState ql;

	printf("an in-progress quest is never dropped to make room\n");
	fresh_log(&ql);
	for(uint32_t id = 1; id <= MAX_QUESTS; id++)
		hold_quest(&ql, id);

	check(!quest_log_make_room(&ql), "no room when everything is in progress");
	check(ql.count == MAX_QUESTS && ql.entries[0].id == 1, "nothing was displaced");
}

static void test_a_long_title_is_cut_not_overrun(void) {
	QuestLogState ql;
	char title[200];

	printf("a title longer than the field is cut\n");
	fresh_log(&ql);
	memset(title, 'x', sizeof(title));
	title[sizeof(title) - 1] = '\0';
	memset(ql.entries[0].title, 'y', sizeof(ql.entries[0].title));
	quest_log_finished_add(&ql, 1, title);

	check(strlen(ql.entries[0].title) == sizeof(ql.entries[0].title) - 1, "cut to the field");
}

int main(void) {
	printf("=== finished quests ===\n");
	test_a_finished_quest_is_listed();
	test_a_quest_under_way_stays_in_progress();
	test_finished_quests_do_not_crowd_out_new_ones();
	test_quests_in_progress_are_never_dropped();
	test_a_long_title_is_cut_not_overrun();
	printf("\n%d checks passed\n", passed);

	return 0;
}
