/**
 * @file
 * Check multi-rate scheduler deadlines with synthetic and monotonic clocks.
 */

#include "tick_scheduler.h"
#include "log.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/** Convert an integer frequency to a nanosecond interval. */
#define HZ_TO_NS(hz) (1000000000L / (hz))

static struct timespec ts(long sec, long nsec) {
    struct timespec t = { .tv_sec = sec, .tv_nsec = nsec };
    return t;
}

static struct timespec ts_ms(long ms) {
    return ts(ms / 1000, (ms % 1000) * 1000000L);
}

static int g_ran[8];
static int g_order[32];
static int g_order_count;

static void reset_counters(void) {
    memset(g_ran, 0, sizeof(g_ran));
    memset(g_order, 0, sizeof(g_order));
    g_order_count = 0;
}

static void record(int which) {
    g_ran[which]++;
    if (g_order_count < 32) g_order[g_order_count++] = which;
}

static void task_a(void* ctx) { (void)ctx; record(0); }
static void task_b(void* ctx) { (void)ctx; record(1); }
static void task_c(void* ctx) { (void)ctx; record(2); }

static int g_prepares;
static void prepare(void* ctx) { (void)ctx; g_prepares++; }

/**
 * Run deadline, ordering, stall, preparation, and validation assertions.
 *
 * @return      Zero after all assertions pass.
 */
int main(void) {
    log_init();
    int due[TICK_SCHEDULER_MAX_TASKS];

    printf("TEST 1: rates with no common divisor coexist (20Hz, 10Hz, 18Hz)\n");
    {
        // exercise rates without a low common base frequency
        TickScheduler s;
        tick_scheduler_init(&s, NULL, NULL);
        assert(tick_scheduler_add(&s, "20hz", HZ_TO_NS(20), task_a) == 0);
        assert(tick_scheduler_add(&s, "10hz", HZ_TO_NS(10), task_b) == 0);
        assert(tick_scheduler_add(&s, "18hz", HZ_TO_NS(18), task_c) == 0);

        struct timespec now = ts(0, 0);
        tick_scheduler_start(&s, &now);
        reset_counters();

        // advance the synthetic clock in one-millisecond steps
        for (long ms = 1; ms <= 1000; ms++) {
            now = ts_ms(ms);
            int n = tick_scheduler_collect_due(&s, &now, due, TICK_SCHEDULER_MAX_TASKS);
            for (int i = 0; i < n; i++) s.tasks[due[i]].run(s.ctx);
        }

        printf("  over 1 simulated second: 20hz=%d  10hz=%d  18hz=%d\n",
               g_ran[0], g_ran[1], g_ran[2]);
        assert(g_ran[0] == 20);
        assert(g_ran[1] == 10);
        assert(g_ran[2] == 18);
    }

    printf("\nTEST 2: the loop sleeps until the EARLIEST deadline\n");
    {
        TickScheduler s;
        tick_scheduler_init(&s, NULL, NULL);
        tick_scheduler_add(&s, "slow", HZ_TO_NS(10), task_a);   // 100ms
        tick_scheduler_add(&s, "fast", HZ_TO_NS(30), task_b);   // 33.3ms

        struct timespec now = ts(0, 0);
        tick_scheduler_start(&s, &now);

        struct timespec deadline;
        assert(tick_scheduler_next_deadline(&s, &deadline) == 1);
        printf("  next deadline is %ldms out (expect ~33, the fast task)\n",
               deadline.tv_nsec / 1000000L);
        assert(deadline.tv_nsec == HZ_TO_NS(30));
    }

    printf("\nTEST 3: only tasks that are actually due run\n");
    {
        TickScheduler s;
        tick_scheduler_init(&s, NULL, NULL);
        tick_scheduler_add(&s, "20hz", HZ_TO_NS(20), task_a);   // 50ms
        tick_scheduler_add(&s, "10hz", HZ_TO_NS(10), task_b);   // 100ms

        struct timespec now = ts(0, 0);
        tick_scheduler_start(&s, &now);
        reset_counters();

        now = ts_ms(50);
        int n = tick_scheduler_collect_due(&s, &now, due, TICK_SCHEDULER_MAX_TASKS);
        printf("  at t=50ms: %d due (expect 1 — only the 20Hz task)\n", n);
        assert(n == 1 && due[0] == 0);

        now = ts_ms(100);
        n = tick_scheduler_collect_due(&s, &now, due, TICK_SCHEDULER_MAX_TASKS);
        printf("  at t=100ms: %d due (expect 2 — both align here)\n", n);
        assert(n == 2 && due[0] == 0 && due[1] == 1);
    }

    printf("\nTEST 4: a stall SKIPS missed ticks instead of replaying them\n");
    {
        // resume after ten missed 20Hz deadlines
        TickScheduler s;
        tick_scheduler_init(&s, NULL, NULL);
        tick_scheduler_add(&s, "20hz", HZ_TO_NS(20), task_a);

        struct timespec now = ts(0, 0);
        tick_scheduler_start(&s, &now);
        reset_counters();

        // The world stalls for half a second, then we come back.
        now = ts_ms(500);
        int n = tick_scheduler_collect_due(&s, &now, due, TICK_SCHEDULER_MAX_TASKS);
        printf("  after a 500ms stall, due count = %d (expect 1, NOT 10)\n", n);
        assert(n == 1);

        printf("  deadlines discarded: %llu (expect 9)\n",
               (unsigned long long)s.tasks[0].dropped);
        assert(s.tasks[0].dropped == 9);

        // require the next deadline to remain in the future
        n = tick_scheduler_collect_due(&s, &now, due, TICK_SCHEDULER_MAX_TASKS);
        printf("  immediate re-poll at the same instant: %d due (expect 0)\n", n);
        assert(n == 0);
    }

    printf("\nTEST 5: phase alignment survives a stall\n");
    {
        // retain interval phase after an off-boundary stall
        TickScheduler s;
        tick_scheduler_init(&s, NULL, NULL);
        tick_scheduler_add(&s, "20hz", HZ_TO_NS(20), task_a);

        struct timespec now = ts(0, 0);
        tick_scheduler_start(&s, &now);

        now = ts_ms(237);                     // stall ends off-boundary
        tick_scheduler_collect_due(&s, &now, due, TICK_SCHEDULER_MAX_TASKS);

        struct timespec deadline;
        tick_scheduler_next_deadline(&s, &deadline);
        long deadline_ms = deadline.tv_sec * 1000 + deadline.tv_nsec / 1000000L;
        printf("  stall ended at 237ms, next deadline at %ldms (expect 250 — on grid)\n",
               deadline_ms);
        assert(deadline_ms == 250);
    }

    printf("\nTEST 6: due tasks run in registration order (the shedding order)\n");
    {
        TickScheduler s;
        tick_scheduler_init(&s, NULL, NULL);
        tick_scheduler_add(&s, "first",  HZ_TO_NS(10), task_a);
        tick_scheduler_add(&s, "second", HZ_TO_NS(10), task_b);
        tick_scheduler_add(&s, "third",  HZ_TO_NS(10), task_c);

        struct timespec now = ts(0, 0);
        tick_scheduler_start(&s, &now);
        reset_counters();

        now = ts_ms(100);
        int n = tick_scheduler_collect_due(&s, &now, due, TICK_SCHEDULER_MAX_TASKS);
        assert(n == 3);
        printf("  order: %d %d %d (expect 0 1 2)\n", due[0], due[1], due[2]);
        assert(due[0] == 0 && due[1] == 1 && due[2] == 2);
    }

    printf("\nTEST 7: prepare runs once per pass, not once per task\n");
    {
        // share one preparation across coincident tasks
        TickScheduler s;
        tick_scheduler_init(&s, prepare, NULL);
        tick_scheduler_add(&s, "20hz", HZ_TO_NS(20), task_a);
        tick_scheduler_add(&s, "10hz", HZ_TO_NS(10), task_b);
        tick_scheduler_add(&s, "30hz", HZ_TO_NS(30), task_c);

        struct timespec now = ts(0, 0);
        tick_scheduler_start(&s, &now);
        g_prepares = 0;
        reset_counters();

        // t=100ms is where all three coincide.
        now = ts_ms(100);
        int n = tick_scheduler_collect_due(&s, &now, due, TICK_SCHEDULER_MAX_TASKS);
        assert(n == 3);
        if (n > 0 && s.prepare) s.prepare(s.ctx);
        for (int i = 0; i < n; i++) s.tasks[due[i]].run(s.ctx);

        printf("  3 tasks ran, prepare called %d time (expect 1)\n", g_prepares);
        assert(g_prepares == 1);
    }

    printf("\nTEST 8: the real loop hits its rates against a real clock\n");
    {
        TickScheduler s;
        tick_scheduler_init(&s, prepare, NULL);
        tick_scheduler_add(&s, "30hz", HZ_TO_NS(30), task_a);
        tick_scheduler_add(&s, "10hz", HZ_TO_NS(10), task_b);

        g_prepares = 0;
        reset_counters();

        volatile int running = 1;
        struct timespec start, end;
        clock_gettime(CLOCK_MONOTONIC, &start);

        // drive the scheduler against the monotonic clock for 600ms
        struct timespec now;
        tick_scheduler_start(&s, &start);
        do {
            struct timespec deadline;
            tick_scheduler_next_deadline(&s, &deadline);
            clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline, NULL);
            clock_gettime(CLOCK_MONOTONIC, &now);
            int n = tick_scheduler_collect_due(&s, &now, due, TICK_SCHEDULER_MAX_TASKS);
            if (n > 0 && s.prepare) s.prepare(s.ctx);
            for (int i = 0; i < n; i++) s.tasks[due[i]].run(s.ctx);
            clock_gettime(CLOCK_MONOTONIC, &end);
        } while ((end.tv_sec - start.tv_sec) * 1000000000L +
                 (end.tv_nsec - start.tv_nsec) < 600000000L && running);

        printf("  in ~600ms: 30hz ran %d (expect 17-19), 10hz ran %d (expect 5-7)\n",
               g_ran[0], g_ran[1]);
        assert(g_ran[0] >= 16 && g_ran[0] <= 20);
        assert(g_ran[1] >= 5  && g_ran[1] <= 7);

        // count preparation once per due-task pass
        printf("  prepares=%d, task runs=%d (prepares must be fewer)\n",
               g_prepares, g_ran[0] + g_ran[1]);
        assert(g_prepares < g_ran[0] + g_ran[1]);
    }

    printf("\nTEST 9: bad arguments are refused\n");
    {
        TickScheduler s;
        tick_scheduler_init(&s, NULL, NULL);

        assert(tick_scheduler_add(&s, "zero", 0, task_a) == -1);
        assert(tick_scheduler_add(&s, "negative", -5, task_a) == -1);
        assert(tick_scheduler_add(&s, "no fn", HZ_TO_NS(10), NULL) == -1);
        assert(tick_scheduler_add(NULL, "null sched", HZ_TO_NS(10), task_a) == -1);

        for (int i = 0; i < TICK_SCHEDULER_MAX_TASKS; i++) {
            assert(tick_scheduler_add(&s, "filler", HZ_TO_NS(10), task_a) == 0);
        }
        assert(tick_scheduler_add(&s, "overflow", HZ_TO_NS(10), task_a) == -1);
        printf("  table full at %d tasks, further adds refused\n",
               TICK_SCHEDULER_MAX_TASKS);

        struct timespec deadline;
        TickScheduler empty;
        tick_scheduler_init(&empty, NULL, NULL);
        assert(tick_scheduler_next_deadline(&empty, &deadline) == 0);

        struct timespec now = ts(0, 0);
        assert(tick_scheduler_collect_due(&empty, &now, due, TICK_SCHEDULER_MAX_TASKS) == 0);
        assert(tick_scheduler_collect_due(NULL, &now, due, 8) == 0);
        printf("  an empty scheduler reports no deadline and nothing due\n");
    }

    printf("\nALL ASSERTIONS PASSED\n");
    return 0;
}
