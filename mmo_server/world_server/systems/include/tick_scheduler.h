#ifndef TICK_SCHEDULER_H
#define TICK_SCHEDULER_H

/** @file Schedule independent-rate nonblocking tasks on one thread.
 * Discard missed deadlines and run due work in registration order.
 */

#include <stdatomic.h>
#include <stdint.h>
#include <time.h>

/** Bound tasks registered with one scheduler. */
#define TICK_SCHEDULER_MAX_TASKS 8

/** Invoke one scheduler task with the shared pass context. */
typedef void (*TickTaskFn)(void* ctx);

/** Track one task's deadline and execution timing. */
typedef struct {
    const char*     name;
    long            interval_ns;
    TickTaskFn      run;
    struct timespec next_due;

    /** Expose execution counters and nanosecond timings for diagnostics. */
    uint64_t        runs;        // times this task actually executed
    uint64_t        dropped;     // deadlines discarded because a pass overran
    long            last_ns;     // duration of the most recent run
    long            worst_ns;    // worst duration seen
} TickTask;

/** Coordinate task deadlines, a shared prepare hook, and pass timing. */
typedef struct {
    TickTask   tasks[TICK_SCHEDULER_MAX_TASKS];
    int        count;

    /** Run once before due tasks to prepare shared pass state. */
    TickTaskFn prepare;
    void*      ctx;

    long       shortest_interval_ns;   // the pass budget
    uint64_t   passes;
    uint64_t   overruns;               // passes that exceeded the budget
    long       worst_pass_ns;
} TickScheduler;

// accept a NULL per-pass prepare hook
void tick_scheduler_init(TickScheduler* scheduler, TickTaskFn prepare, void* ctx);

// retain name and return -1 for capacity or invalid arguments
int tick_scheduler_add(TickScheduler* scheduler, const char* name,
                       long interval_ns, TickTaskFn run);

void tick_scheduler_start(TickScheduler* scheduler, const struct timespec* now);

// return zero when no task deadline exists
int tick_scheduler_next_deadline(const TickScheduler* scheduler,
                                 struct timespec* out_deadline);

// collect in registration order while advancing past missed intervals
int tick_scheduler_collect_due(TickScheduler* scheduler,
                               const struct timespec* now,
                               int* out_indices, int max_out);

// dispatch until cleared with shutdown latency bounded by the shortest interval
/* _Atomic, not volatile: the flag is cleared by a signal handler and read here
 * on the broadcast thread, which volatile does not order. */
void tick_scheduler_run(TickScheduler* scheduler, const _Atomic int* keep_running);

void tick_scheduler_report(const TickScheduler* scheduler);

#endif // TICK_SCHEDULER_H
