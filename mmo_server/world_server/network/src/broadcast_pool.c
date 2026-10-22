/**
 * @file
 * Run broadcast passes across a fixed set of shard workers.
 */

#include "broadcast_pool.h"

#include "log.h"

#include <pthread.h>
#include <string.h>

/** Coordinate one generation of shard work across the pool.
 *
 * Workers wait for `generation` to advance, run their shard, then report by
 * incrementing `finished`. The caller waits for `finished` to reach the worker
 * count. One mutex and two condition variables, because a pass is short and
 * frequent: anything that parks a thread for longer than the pass itself would
 * cost more than the serialisation it replaces.
 */
static struct {
    pthread_t       threads[BROADCAST_POOL_MAX_SHARDS];
    int             thread_count;      /**< Workers, one fewer than shard_count. */
    int             shard_count;

    pthread_mutex_t lock;
    pthread_cond_t  work_ready;        /**< Signalled when a generation begins. */
    pthread_cond_t  work_done;         /**< Signalled as each worker finishes. */

    BroadcastShardFn fn;
    void*            ctx;
    unsigned long    generation;
    int              finished;

    int running;
    int started;
} g_pool;

/** Wait for generations and run this worker's shard of each. */
static void* shard_worker(void* arg) {
    const int shard = (int)(long)arg;
    unsigned long seen = 0;

    for (;;) {
        pthread_mutex_lock(&g_pool.lock);
        while (g_pool.running && g_pool.generation == seen)
            pthread_cond_wait(&g_pool.work_ready, &g_pool.lock);

        if (!g_pool.running) {
            pthread_mutex_unlock(&g_pool.lock);
            return NULL;
        }

        seen = g_pool.generation;
        BroadcastShardFn fn = g_pool.fn;
        void* ctx = g_pool.ctx;
        int shard_count = g_pool.shard_count;
        pthread_mutex_unlock(&g_pool.lock);

        if (fn) fn(ctx, shard, shard_count);

        pthread_mutex_lock(&g_pool.lock);
        g_pool.finished++;
        pthread_cond_signal(&g_pool.work_done);
        pthread_mutex_unlock(&g_pool.lock);
    }
}

/**
 * Start the pool.
 *
 * @param shard_count  Desired shards, clamped to [1, BROADCAST_POOL_MAX_SHARDS].
 * @return 1 on success, or 0 when threads could not be created.
 */
int broadcast_pool_start(int shard_count) {
    if (g_pool.started) return 1;

    if (shard_count < 1) shard_count = 1;
    if (shard_count > BROADCAST_POOL_MAX_SHARDS) shard_count = BROADCAST_POOL_MAX_SHARDS;

    memset(&g_pool, 0, sizeof(g_pool));
    g_pool.shard_count = shard_count;
    g_pool.running = 1;

    pthread_mutex_init(&g_pool.lock, NULL);
    pthread_cond_init(&g_pool.work_ready, NULL);
    pthread_cond_init(&g_pool.work_done, NULL);

    // Shard 0 runs on the calling thread, so only the rest need workers.
    for (int i = 1; i < shard_count; i++) {
        if (pthread_create(&g_pool.threads[g_pool.thread_count], NULL,
                           shard_worker, (void*)(long)i) != 0) {
            LOG_ERROR("[BROADCAST] could not start shard worker %d — "
                      "continuing with %d shards", i, i);
            g_pool.shard_count = i;   // run with what actually started
            break;
        }
        g_pool.thread_count++;
    }

    g_pool.started = 1;
    LOG_INFO("[BROADCAST] fan-out pool: %d shards (%d workers + the pass thread)",
             g_pool.shard_count, g_pool.thread_count);
    return 1;
}

/**
 * Stop the pool and join its workers.
 */
void broadcast_pool_stop(void) {
    if (!g_pool.started) return;

    pthread_mutex_lock(&g_pool.lock);
    g_pool.running = 0;
    pthread_cond_broadcast(&g_pool.work_ready);
    pthread_mutex_unlock(&g_pool.lock);

    for (int i = 0; i < g_pool.thread_count; i++)
        pthread_join(g_pool.threads[i], NULL);

    pthread_mutex_destroy(&g_pool.lock);
    pthread_cond_destroy(&g_pool.work_ready);
    pthread_cond_destroy(&g_pool.work_done);

    memset(&g_pool, 0, sizeof(g_pool));
}

/**
 * Report the active shard count.
 *
 * @return Shards in use, or 1 when the pool is not running.
 */
int broadcast_pool_shards(void) {
    return g_pool.started ? g_pool.shard_count : 1;
}

/**
 * Run one function across every shard and return once all shards have finished.
 */
void broadcast_pool_run(BroadcastShardFn fn, void* ctx) {
    if (!fn) return;

    // Not started, or nothing to fan out to: just do the work here.
    if (!g_pool.started || g_pool.thread_count == 0) {
        fn(ctx, 0, g_pool.started ? g_pool.shard_count : 1);
        return;
    }

    pthread_mutex_lock(&g_pool.lock);
    g_pool.fn       = fn;
    g_pool.ctx      = ctx;
    g_pool.finished = 0;
    g_pool.generation++;
    pthread_cond_broadcast(&g_pool.work_ready);
    pthread_mutex_unlock(&g_pool.lock);

    // Shard 0 on this thread, rather than blocking while a worker does it.
    fn(ctx, 0, g_pool.shard_count);

    pthread_mutex_lock(&g_pool.lock);
    while (g_pool.finished < g_pool.thread_count)
        pthread_cond_wait(&g_pool.work_done, &g_pool.lock);
    pthread_mutex_unlock(&g_pool.lock);
}
