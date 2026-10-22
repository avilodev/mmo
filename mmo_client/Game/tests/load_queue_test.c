// Check the step queue the loading screen is driven by.
#include "states/load_queue.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static int g_order[8];
static int g_ran;

static int record_ok(void* ctx) {
	g_order[g_ran++] = *(int*)ctx;
	return 1;
}

static int record_fail(void* ctx) {
	g_order[g_ran++] = *(int*)ctx;
	return 0;
}

static void reset_record(void) {
	memset(g_order, 0, sizeof(g_order));
	g_ran = 0;
}

static void test_empty_queue_is_finished(void) {
	load_queue q;

	load_queue_init(&q);
	assert(load_queue_finished(&q));
	assert(!load_queue_failed(&q));
	assert(load_queue_progress(&q) == 1.0f);
	assert(load_queue_step(&q, 0.0) == 0);
}

static void test_steps_run_in_order_one_per_call(void) {
	load_queue q;
	int ids[3] = { 1, 2, 3 };

	reset_record();
	load_queue_init(&q);
	for(int i = 0; i < 3; i++)
		assert(load_queue_add(&q, "step", 1, 1, 0.0, record_ok, &ids[i]));

	assert(!load_queue_finished(&q));
	assert(load_queue_step(&q, 0.0) == 1);
	assert(g_ran == 1 && g_order[0] == 1);
	assert(load_queue_step(&q, 0.0) == 1);
	assert(load_queue_step(&q, 0.0) == 1);
	assert(g_ran == 3 && g_order[1] == 2 && g_order[2] == 3);
	assert(load_queue_finished(&q));
	assert(load_queue_step(&q, 0.0) == 0);
	assert(g_ran == 3);
}

static void test_progress_follows_weights(void) {
	load_queue q;
	int id = 0;

	load_queue_init(&q);
	load_queue_add(&q, "small", 1, 1, 0.0, record_ok, &id);
	load_queue_add(&q, "big", 3, 1, 0.0, record_ok, &id);

	assert(load_queue_progress(&q) == 0.0f);
	load_queue_step(&q, 0.0);
	assert(load_queue_progress(&q) == 0.25f);
	load_queue_step(&q, 0.0);
	assert(load_queue_progress(&q) == 1.0f);
}

static void test_label_names_the_step_about_to_run(void) {
	load_queue q;
	int id = 0;

	load_queue_init(&q);
	load_queue_add(&q, "first", 1, 1, 0.0, record_ok, &id);
	load_queue_add(&q, "second", 1, 1, 0.0, record_ok, &id);

	assert(strcmp(load_queue_label(&q), "first") == 0);
	load_queue_step(&q, 0.0);
	assert(strcmp(load_queue_label(&q), "second") == 0);
	load_queue_step(&q, 0.0);
	assert(strcmp(load_queue_label(&q), "Ready") == 0);
}

static void test_optional_failure_carries_on(void) {
	load_queue q;
	int ids[3] = { 1, 2, 3 };

	reset_record();
	load_queue_init(&q);
	load_queue_add(&q, "a", 1, 1, 0.0, record_ok, &ids[0]);
	load_queue_add(&q, "b", 1, 0, 0.0, record_fail, &ids[1]);
	load_queue_add(&q, "c", 1, 1, 0.0, record_ok, &ids[2]);

	while(load_queue_step(&q, 0.0))
		;
	assert(g_ran == 3);
	assert(!load_queue_failed(&q));
	assert(load_queue_skipped(&q) == 1);
	assert(load_queue_progress(&q) == 1.0f);
}

static void test_required_failure_stops_the_queue(void) {
	load_queue q;
	int ids[3] = { 1, 2, 3 };

	reset_record();
	load_queue_init(&q);
	load_queue_add(&q, "a", 1, 1, 0.0, record_ok, &ids[0]);
	load_queue_add(&q, "b", 1, 1, 0.0, record_fail, &ids[1]);
	load_queue_add(&q, "c", 1, 1, 0.0, record_ok, &ids[2]);

	while(load_queue_step(&q, 0.0))
		;
	assert(g_ran == 2);
	assert(load_queue_failed(&q));
	assert(load_queue_finished(&q));
	assert(strcmp(load_queue_label(&q), "b") == 0);
	assert(load_queue_progress(&q) < 1.0f);
}

static void test_refuses_bad_steps_and_overflow(void) {
	load_queue q;
	int id = 0;

	load_queue_init(&q);
	assert(!load_queue_add(&q, "no fn", 1, 1, 0.0, NULL, &id));
	assert(!load_queue_add(&q, "no weight", 0, 1, 0.0, record_ok, &id));
	for(int i = 0; i < LOAD_QUEUE_MAX_STEPS; i++)
		assert(load_queue_add(&q, "step", 1, 1, 0.0, record_ok, &id));
	assert(!load_queue_add(&q, "one too many", 1, 1, 0.0, record_ok, &id));
}


static int g_answer;

static int answer_step(void* ctx) {
	(void)ctx;
	return g_answer;
}

static void test_waiting_step_stays_current_until_done(void) {
	load_queue q;
	int id = 7;

	reset_record();
	load_queue_init(&q);
	load_queue_add(&q, "wait", 1, 1, 0.0, answer_step, NULL);
	load_queue_add(&q, "after", 1, 1, 0.0, record_ok, &id);

	g_answer = LOAD_STEP_WAIT;
	assert(load_queue_step(&q, 0.0) == 1);
	assert(load_queue_step(&q, 100.0) == 1); // no timeout set: waits for ever
	assert(!load_queue_finished(&q));
	assert(strcmp(load_queue_label(&q), "wait") == 0);
	assert(g_ran == 0);
	assert(load_queue_progress(&q) == 0.0f);

	g_answer = LOAD_STEP_DONE;
	load_queue_step(&q, 101.0);
	assert(strcmp(load_queue_label(&q), "after") == 0);
	assert(load_queue_progress(&q) == 0.5f);
	load_queue_step(&q, 102.0);
	assert(load_queue_finished(&q) && !load_queue_failed(&q));
}

static void test_wait_timeout_counts_from_the_first_run(void) {
	load_queue q;

	load_queue_init(&q);
	load_queue_add(&q, "slow", 1, 0, 5.0, answer_step, NULL);

	g_answer = LOAD_STEP_WAIT;
	load_queue_step(&q, 10.0);
	load_queue_step(&q, 14.9);
	assert(!load_queue_finished(&q));
	assert(!load_queue_last_timed_out(&q));
	load_queue_step(&q, 15.0);
	assert(load_queue_finished(&q));
	assert(load_queue_last_timed_out(&q));
	assert(load_queue_skipped(&q) == 1);
	assert(!load_queue_failed(&q));
}

static void test_required_wait_timeout_fails_the_load(void) {
	load_queue q;

	load_queue_init(&q);
	load_queue_add(&q, "needed", 1, 1, 2.0, answer_step, NULL);

	g_answer = LOAD_STEP_WAIT;
	load_queue_step(&q, 0.0);
	load_queue_step(&q, 2.5);
	assert(load_queue_failed(&q));
	assert(load_queue_last_timed_out(&q));
	assert(strcmp(load_queue_label(&q), "needed") == 0);
}

static void test_each_step_gets_its_own_clock(void) {
	load_queue q;

	load_queue_init(&q);
	load_queue_add(&q, "first", 1, 1, 3.0, answer_step, NULL);
	load_queue_add(&q, "second", 1, 1, 3.0, answer_step, NULL);

	g_answer = LOAD_STEP_WAIT;
	load_queue_step(&q, 0.0);
	g_answer = LOAD_STEP_DONE;
	load_queue_step(&q, 2.0); // first done at 2.0
	g_answer = LOAD_STEP_WAIT;
	load_queue_step(&q, 4.0); // second starts at 4.0, not 0.0
	load_queue_step(&q, 6.9);
	assert(!load_queue_finished(&q));
	load_queue_step(&q, 7.0);
	assert(load_queue_failed(&q));
}

int main(void) {
	test_empty_queue_is_finished();
	test_steps_run_in_order_one_per_call();
	test_progress_follows_weights();
	test_label_names_the_step_about_to_run();
	test_optional_failure_carries_on();
	test_required_failure_stops_the_queue();
	test_refuses_bad_steps_and_overflow();
	test_waiting_step_stays_current_until_done();
	test_wait_timeout_counts_from_the_first_run();
	test_required_wait_timeout_fails_the_load();
	test_each_step_gets_its_own_clock();
	printf("load_queue_test: all tests passed\n");
	return 0;
}
