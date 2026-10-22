/**
 * @file
 * Which step of a quest the player is on, and how it is worded.
 *
 * The server owns the step. The client keeps what it is told and says it in
 * the log, the tracker and the map, so what matters here is that a new step
 * starts from nothing, that a resend of the same step costs no progress, and
 * that a quest with no named steps reads exactly as it always did.
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
static QuestEntry* hold_quest(QuestLogState* ql, uint32_t id, const char* title) {
	QuestEntry* e = &ql->entries[ql->count++];

	memset(e, 0, sizeof(*e));
	e->id = id;
	e->active = 1;
	e->obj_count = 1;
	snprintf(e->title, sizeof(e->title), "%s", title);

	return e;
}

static void test_the_step_is_recorded_on_the_right_quest(void) {
	QuestLogState ql;

	printf("a step is recorded on the quest it names and no other\n");
	fresh_log(&ql);
	hold_quest(&ql, 301, "Welcome to Ennara");
	hold_quest(&ql, 400, "Wolves");
	quest_log_set_step(&ql, 301, 2, 7, "Honest Goods");

	check(ql.entries[0].step == 2, "the step");
	check(ql.entries[0].step_count == 7, "the step count");
	check(strcmp(ql.entries[0].step_title, "Honest Goods") == 0, "the step title");
	check(ql.entries[1].step == 0 && ql.entries[1].step_count == 0, "another quest is untouched");

	quest_log_set_step(&ql, 999, 3, 3, "Nobody");
	check(ql.count == 2, "an unknown quest adds nothing");
}

static void test_a_new_step_starts_from_nothing(void) {
	QuestLogState ql;
	QuestEntry* e;

	printf("a new step clears progress; a resend of the same step keeps it\n");
	fresh_log(&ql);
	e = hold_quest(&ql, 301, "Welcome to Ennara");
	quest_log_set_step(&ql, 301, 2, 7, "Honest Goods");
	e->objectives[0].current = 1;

	// a reconnect resends the step the player is already on
	quest_log_set_step(&ql, 301, 2, 7, "Honest Goods");
	check(e->objectives[0].current == 1, "a resend of the same step lost progress");

	quest_log_set_step(&ql, 301, 3, 7, "A Fair Price");
	check(e->objectives[0].current == 0, "the next step began with the last one's progress");
	check(strcmp(e->step_title, "A Fair Price") == 0, "the next step's name");
}

static void test_a_one_step_quest_reads_as_it_always_did(void) {
	QuestLogState ql;
	QuestEntry* e;
	char text[64] = "stale";

	printf("a quest with no named steps says nothing about steps\n");
	fresh_log(&ql);
	e = hold_quest(&ql, 400, "Wolves");
	quest_log_set_step(&ql, 400, 1, 1, "");

	check(quest_step_label(e, text, sizeof(text)) == 0 && text[0] == '\0', "no label");
	strcpy(text, "stale");
	check(quest_step_tag(e, text, sizeof(text)) == 0 && text[0] == '\0', "no tag");

	// a quest the server has not described yet is the same
	e->step_count = 0;
	check(quest_step_label(e, text, sizeof(text)) == 0, "no label before the server speaks");
}

static void test_a_multi_step_quest_names_its_step(void) {
	QuestLogState ql;
	QuestEntry* e;
	char text[64];

	printf("a quest with steps says which one the player is on\n");
	fresh_log(&ql);
	e = hold_quest(&ql, 301, "Welcome to Ennara");
	quest_log_set_step(&ql, 301, 2, 7, "Honest Goods");

	check(quest_step_label(e, text, sizeof(text)) == 1, "a label is written");
	check(strcmp(text, "Step 2 of 7: Honest Goods") == 0, "the label reads well");
	check(quest_step_tag(e, text, sizeof(text)) == 1, "a tag is written");
	check(strcmp(text, "2/7") == 0, "the tag reads well");

	// an unnamed step is still counted
	quest_log_set_step(&ql, 301, 3, 7, "");
	check(quest_step_label(e, text, sizeof(text)) == 1, "an unnamed step still has a label");
	check(strcmp(text, "Step 3 of 7") == 0, "the label has no dangling colon");
}

static void test_wording_cannot_overrun_its_buffer(void) {
	QuestLogState ql;
	QuestEntry* e;
	char tiny[8];
	char title[sizeof(e->step_title) + 20];

	printf("a long step name is cut to fit, not written past the buffer\n");
	fresh_log(&ql);
	e = hold_quest(&ql, 301, "Welcome to Ennara");
	memset(title, 'A', sizeof(title) - 1);
	title[sizeof(title) - 1] = '\0';
	quest_log_set_step(&ql, 301, 1, 7, title);

	check(strlen(e->step_title) == sizeof(e->step_title) - 1, "the title is cut and ended");
	memset(tiny, 'x', sizeof(tiny));
	quest_step_label(e, tiny, sizeof(tiny));
	check(memchr(tiny, '\0', sizeof(tiny)) != NULL, "a small buffer is still ended");
}

int main(void) {
	printf("=== quest steps ===\n");
	test_the_step_is_recorded_on_the_right_quest();
	test_a_new_step_starts_from_nothing();
	test_a_one_step_quest_reads_as_it_always_did();
	test_a_multi_step_quest_names_its_step();
	test_wording_cannot_overrun_its_buffer();
	printf("\n%d checks passed\n", passed);

	return 0;
}
