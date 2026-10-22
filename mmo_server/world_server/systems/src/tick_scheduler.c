/**
 * @file
 * Schedule independent periodic tasks by monotonic absolute deadlines on one thread.
 */

#include "tick_scheduler.h"
#include "log.h"

#include <stddef.h>
#include <string.h>

#define NS_PER_SEC 1000000000L

static void ts_add_ns(struct timespec* t, long ns) {
    t->tv_sec  += ns / NS_PER_SEC;
    t->tv_nsec += ns % NS_PER_SEC;
    if (t->tv_nsec >= NS_PER_SEC) {
        t->tv_sec++;
        t->tv_nsec -= NS_PER_SEC;
    } else if (t->tv_nsec < 0) {
        t->tv_sec--;
        t->tv_nsec += NS_PER_SEC;
    }
}

// a <= b
static int ts_at_or_before(const struct timespec* a, const struct timespec* b) {
    if (a->tv_sec != b->tv_sec) return a->tv_sec < b->tv_sec;
    return a->tv_nsec <= b->tv_nsec;
}

// a - b, in nanoseconds. tv_sec is 64-bit here, so the product cannot realistically
// overflow for any interval this scheduler deals with.
static long ts_diff_ns(const struct timespec* a, const struct timespec* b) {
    return (a->tv_sec - b->tv_sec) * NS_PER_SEC + (a->tv_nsec - b->tv_nsec);
}

/**
 * Reset a scheduler and assign its optional per-pass preparation callback.
 */
void tick_scheduler_init(TickScheduler* scheduler, TickTaskFn prepare, void* ctx) {
    if (!scheduler) return;
    memset(scheduler, 0, sizeof(*scheduler));
    scheduler->prepare = prepare;
    scheduler->ctx = ctx;
}

/**
 * Register a periodic task in dispatch order.
 *
 * The task name must outlive the scheduler.
 *
 * @param interval_ns  Positive task period in nanoseconds.
 * @param run          Task callback; may not be NULL.
 * @return             0 on success, or -1 for invalid input or a full scheduler.
 */
int tick_scheduler_add(TickScheduler* scheduler, const char* name,
                       long interval_ns, TickTaskFn run) {
    if (!scheduler || !run || !name) return -1;
    if (interval_ns <= 0) return -1;
    if (scheduler->count >= TICK_SCHEDULER_MAX_TASKS) return -1;

    TickTask* task = &scheduler->tasks[scheduler->count++];
    memset(task, 0, sizeof(*task));
    task->name = name;
    task->interval_ns = interval_ns;
    task->run = run;

    // The pass budget is set by the fastest task: every pass has to finish
    // before the soonest possible next deadline.
    if (scheduler->shortest_interval_ns == 0 ||
        interval_ns < scheduler->shortest_interval_ns) {
        scheduler->shortest_interval_ns = interval_ns;
    }
    return 0;
}

/**
 * Seed each task deadline one interval after a reference time.
 */
void tick_scheduler_start(TickScheduler* scheduler, const struct timespec* now) {
    if (!scheduler || !now) return;
    for (int i = 0; i < scheduler->count; i++) {
        scheduler->tasks[i].next_due = *now;
        ts_add_ns(&scheduler->tasks[i].next_due, scheduler->tasks[i].interval_ns);
    }
}

/**
 * Find the earliest registered task deadline.
 *
 * @param out_deadline  Receives the deadline.
 * @return              1 when a task exists, or 0 otherwise.
 */
int tick_scheduler_next_deadline(const TickScheduler* scheduler,
                                 struct timespec* out_deadline) {
    if (!scheduler || !out_deadline || scheduler->count == 0) return 0;

    struct timespec earliest = scheduler->tasks[0].next_due;
    for (int i = 1; i < scheduler->count; i++) {
        if (ts_at_or_before(&scheduler->tasks[i].next_due, &earliest)) {
            earliest = scheduler->tasks[i].next_due;
        }
    }
    *out_deadline = earliest;
    return 1;
}

/**
 * Collect due tasks and advance their deadlines past the current time.
 *
 * Missed intervals are counted and discarded while preserving task phase.
 *
 * @param out_indices  Receives task indices in registration order.
 * @param max_out      Output capacity.
 * @return             The number collected.
 */
int tick_scheduler_collect_due(TickScheduler* scheduler,
                               const struct timespec* now,
                               int* out_indices, int max_out) {
    if (!scheduler || !now || !out_indices || max_out <= 0) return 0;

    int due_count = 0;

    for (int i = 0; i < scheduler->count && due_count < max_out; i++) {
        TickTask* task = &scheduler->tasks[i];
        if (!ts_at_or_before(&task->next_due, now)) continue;

        out_indices[due_count++] = i;

        // advance by whole intervals to preserve phase
        ts_add_ns(&task->next_due, task->interval_ns);
        while (ts_at_or_before(&task->next_due, now)) {
            ts_add_ns(&task->next_due, task->interval_ns);
            task->dropped++;
        }
    }

    return due_count;
}

/**
 * Sleep to absolute deadlines and dispatch due tasks until shutdown.
 *
 * Runs every callback serially; a blocking task delays every later task in the pass.
 *
 * @param keep_running  Volatile flag whose zero value requests shutdown.
 */
void tick_scheduler_run(TickScheduler* scheduler, const _Atomic int* keep_running) {
    if (!scheduler || !keep_running || scheduler->count == 0) return;

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    tick_scheduler_start(scheduler, &now);

    while (*keep_running) {
        struct timespec deadline;
        if (!tick_scheduler_next_deadline(scheduler, &deadline)) return;

        // Absolute-time sleep, so a deadline already in the past returns
        // immediately rather than adding another interval of latency.
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline, NULL);
        if (!*keep_running) break;

        clock_gettime(CLOCK_MONOTONIC, &now);

        int due[TICK_SCHEDULER_MAX_TASKS];
        int due_count = tick_scheduler_collect_due(scheduler, &now, due,
                                                   TICK_SCHEDULER_MAX_TASKS);
        if (due_count == 0) continue;

        struct timespec pass_start;
        clock_gettime(CLOCK_MONOTONIC, &pass_start);

        // prepare one shared snapshot for the pass
        if (scheduler->prepare) scheduler->prepare(scheduler->ctx);

        for (int i = 0; i < due_count; i++) {
            TickTask* task = &scheduler->tasks[due[i]];

            struct timespec task_start, task_end;
            clock_gettime(CLOCK_MONOTONIC, &task_start);
            task->run(scheduler->ctx);
            clock_gettime(CLOCK_MONOTONIC, &task_end);

            task->last_ns = ts_diff_ns(&task_end, &task_start);
            if (task->last_ns > task->worst_ns) task->worst_ns = task->last_ns;
            task->runs++;

            // rate-limit reports for tasks exceeding their period
            if (task->last_ns > task->interval_ns) {
                LOG_WARN_RL(5, 60, "[TICK] task '%s' took %ldms, over its %ldms period",
                            task->name,
                            task->last_ns / 1000000L,
                            task->interval_ns / 1000000L);
            }
        }

        struct timespec pass_end;
        clock_gettime(CLOCK_MONOTONIC, &pass_end);
        long pass_ns = ts_diff_ns(&pass_end, &pass_start);
        if (pass_ns > scheduler->worst_pass_ns) scheduler->worst_pass_ns = pass_ns;
        scheduler->passes++;

        if (pass_ns > scheduler->shortest_interval_ns) {
            scheduler->overruns++;
            LOG_WARN_RL(5, 60, "[TICK] pass took %ldms, over the %ldms budget "
                               "(%d tasks) — streams will slip together",
                        pass_ns / 1000000L,
                        scheduler->shortest_interval_ns / 1000000L,
                        due_count);
        }
    }
}

/**
 * Log aggregate scheduler and per-task timing counters.
 */
void tick_scheduler_report(const TickScheduler* scheduler) {
    if (!scheduler) return;

    LOG_INFO("[TICK] %llu passes, %llu over budget (worst %ldms / %ldms)",
             (unsigned long long)scheduler->passes,
             (unsigned long long)scheduler->overruns,
             scheduler->worst_pass_ns / 1000000L,
             scheduler->shortest_interval_ns / 1000000L);

    for (int i = 0; i < scheduler->count; i++) {
        const TickTask* task = &scheduler->tasks[i];
        LOG_INFO("[TICK]   %-12s %5ldms  runs=%llu dropped=%llu worst=%ldms",
                 task->name,
                 task->interval_ns / 1000000L,
                 (unsigned long long)task->runs,
                 (unsigned long long)task->dropped,
                 task->worst_ns / 1000000L);
    }
}
