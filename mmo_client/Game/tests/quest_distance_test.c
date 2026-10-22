// Check how far away a quest is said to be.

#include "ui/quest_distance.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static QuestEntry quest_with(int count) {
	QuestEntry q;

	memset(&q, 0, sizeof(q));
	q.obj_count = (uint8_t)count;
	return q;
}

static void test_an_active_quest_points_at_its_first_unfinished_place(void) {
	QuestEntry q = quest_with(3);
	float x = 0.0f;
	float y = 0.0f;

	q.objectives[0].required = 1;
	q.objectives[0].current = 1;
	q.objectives[0].has_marker = 1;
	q.objectives[0].marker_x = 10.0f;
	q.objectives[1].required = 1;
	q.objectives[1].has_marker = 1;
	q.objectives[1].marker_x = 200.0f;
	q.objectives[1].marker_y = 300.0f;
	q.objectives[2].required = 1;
	q.objectives[2].has_marker = 1;
	q.objectives[2].marker_x = 999.0f;

	assert(quest_active_target(&q, &x, &y));
	assert(x == 200.0f && y == 300.0f);
}

static void test_an_objective_with_no_place_is_skipped(void) {
	QuestEntry q = quest_with(2);
	float x = 0.0f;
	float y = 0.0f;

	q.objectives[0].required = 1;
	q.objectives[1].required = 1;
	q.objectives[1].has_marker = 1;
	q.objectives[1].marker_x = 5.0f;
	q.objectives[1].marker_y = 6.0f;

	assert(quest_active_target(&q, &x, &y));
	assert(x == 5.0f && y == 6.0f);
}

static void test_a_quest_with_no_place_has_no_distance(void) {
	QuestEntry q = quest_with(1);
	float x;
	float y;

	q.objectives[0].required = 1;
	assert(!quest_active_target(&q, &x, &y));

	// Everything done: nowhere is named to hand it in
	q.objectives[0].has_marker = 1;
	q.objectives[0].current = 1;
	assert(!quest_active_target(&q, &x, &y));

	q = quest_with(0);
	assert(!quest_active_target(&q, &x, &y));
}

static void test_an_available_quest_points_at_its_giver(void) {
	available_quest_entry q;
	float x = 0.0f;
	float y = 0.0f;

	memset(&q, 0, sizeof(q));
	assert(!quest_available_target(&q, &x, &y));

	q.has_giver_pos = 1;
	q.giver_x = 40.0f;
	q.giver_y = 50.0f;
	assert(quest_available_target(&q, &x, &y));
	assert(x == 40.0f && y == 50.0f);
}

static void test_distance_is_in_metres(void) {
	assert(fabsf(quest_distance_m(0.0f, 0.0f, 0.0f, 0.0f)) < 0.001f);
	assert(fabsf(quest_distance_m(0.0f, 0.0f, 3.0f * QUEST_UNITS_PER_METRE,
	                              4.0f * QUEST_UNITS_PER_METRE) - 5.0f) < 0.01f);
	assert(fabsf(quest_distance_m(100.0f, 50.0f, 100.0f, 50.0f + 40.0f) - 1.83f) < 0.01f);
}

static void test_distance_text(void) {
	char text[16];

	quest_distance_text(0.0f, text, sizeof(text));
	assert(strcmp(text, "0 m") == 0);
	quest_distance_text(84.6f, text, sizeof(text));
	assert(strcmp(text, "85 m") == 0);
	quest_distance_text(999.4f, text, sizeof(text));
	assert(strcmp(text, "999 m") == 0);
	quest_distance_text(1000.0f, text, sizeof(text));
	assert(strcmp(text, "1.0 km") == 0);
	quest_distance_text(1234.0f, text, sizeof(text));
	assert(strcmp(text, "1.2 km") == 0);
	quest_distance_text(25000.0f, text, sizeof(text));
	assert(strcmp(text, "25 km") == 0);
	quest_distance_text(-3.0f, text, sizeof(text));
	assert(strcmp(text, "0 m") == 0);
}

static void test_text_fits_a_small_buffer(void) {
	char text[4];

	quest_distance_text(123456.0f, text, sizeof(text));
	assert(strlen(text) == 3);
}

int main(void) {
	test_an_active_quest_points_at_its_first_unfinished_place();
	test_an_objective_with_no_place_is_skipped();
	test_a_quest_with_no_place_has_no_distance();
	test_an_available_quest_points_at_its_giver();
	test_distance_is_in_metres();
	test_distance_text();
	test_text_fits_a_small_buffer();
	printf("quest_distance_test: all passed\n");
	return 0;
}
