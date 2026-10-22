#ifndef LOAD_QUEUE_H
#define LOAD_QUEUE_H

// An ordered list of load steps, run one per call so a screen can draw between.
// A step can also wait for something to arrive, and give up after a time.
// No GL here, so it can be tested on its own.

#define LOAD_QUEUE_MAX_STEPS 24

// What a step answers each time it is run
#define LOAD_STEP_FAILED 0
#define LOAD_STEP_DONE 1
#define LOAD_STEP_WAIT 2 // not yet; the same step is run again next call

typedef int (*load_step_fn)(void* ctx);

typedef struct {
	const char* label; // shown while the step runs
	int weight; // share of the progress bar
	int required; // a failure here ends the load
	double timeout; // seconds a waiting step may wait; 0 waits forever
	load_step_fn run;
	void* ctx;
} load_step;

typedef struct {
	load_step steps[LOAD_QUEUE_MAX_STEPS];
	int count;
	int next;
	int total_weight;
	int done_weight;
	int skipped;
	int failed;
	int waiting; // the current step has been run at least once
	double wait_started;
	int last_timed_out; // the step that just ended did so by timing out
	const char* failed_label;
} load_queue;

void load_queue_init(load_queue* q);

// Returns 1 if the step was queued; 0 if the queue is full or the step is bad
int load_queue_add(load_queue* q, const char* label, int weight, int required,
                   double timeout, load_step_fn run, void* ctx);

// Run the current step at time now. Returns 1 if one ran, 0 if none was left.
// A step that waits stays current until it is done, fails, or times out.
int load_queue_step(load_queue* q, double now);

// Nonzero once every step has finished, or a required one failed
int load_queue_finished(const load_queue* q);

// Nonzero if a required step failed
int load_queue_failed(const load_queue* q);

// How many optional steps failed or timed out and were passed over
int load_queue_skipped(const load_queue* q);

// Nonzero if the step that last ended did so by running out of time
int load_queue_last_timed_out(const load_queue* q);

// 0 to 1, by step weight
float load_queue_progress(const load_queue* q);

// The step in hand, the one that failed, or "Ready" when all is done
const char* load_queue_label(const load_queue* q);

#endif /* LOAD_QUEUE_H */
