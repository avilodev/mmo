// Run load steps one at a time and report how far along they are.

#include "states/load_queue.h"

#include <string.h>

void load_queue_init(load_queue* q) {
	memset(q, 0, sizeof(*q));
}

int load_queue_add(load_queue* q, const char* label, int weight, int required,
                   double timeout, load_step_fn run, void* ctx) {
	load_step* step;

	if(q->count >= LOAD_QUEUE_MAX_STEPS || !run || weight < 1)
		return 0;
	step = &q->steps[q->count++];
	step->label = label;
	step->weight = weight;
	step->required = required;
	step->timeout = timeout;
	step->run = run;
	step->ctx = ctx;
	q->total_weight += weight;
	return 1;
}

int load_queue_finished(const load_queue* q) {
	return q->failed || q->next >= q->count;
}

int load_queue_failed(const load_queue* q) {
	return q->failed;
}

int load_queue_skipped(const load_queue* q) {
	return q->skipped;
}

int load_queue_last_timed_out(const load_queue* q) {
	return q->last_timed_out;
}

int load_queue_step(load_queue* q, double now) {
	load_step* step;
	int result;

	if(load_queue_finished(q))
		return 0;
	step = &q->steps[q->next];
	if(!q->waiting) {
		q->waiting = 1;
		q->wait_started = now;
	}
	q->last_timed_out = 0;
	result = step->run(step->ctx);
	if(result == LOAD_STEP_WAIT) {
		if(step->timeout <= 0.0 || now - q->wait_started < step->timeout)
			return 1;
		q->last_timed_out = 1;
		result = LOAD_STEP_FAILED;
	}
	q->waiting = 0;
	q->next++;
	if(result == LOAD_STEP_DONE) {
		q->done_weight += step->weight;
	} else if(step->required) {
		q->failed = 1;
		q->failed_label = step->label;
	} else {
		q->skipped++;
		q->done_weight += step->weight;
	}
	return 1;
}

float load_queue_progress(const load_queue* q) {
	if(q->total_weight <= 0)
		return 1.0f;
	return (float)q->done_weight / (float)q->total_weight;
}

const char* load_queue_label(const load_queue* q) {
	if(q->failed)
		return q->failed_label;
	if(q->next >= q->count)
		return "Ready";
	return q->steps[q->next].label;
}
