/**
 * @file
 * Check that the broadcast fan-out pool covers every descriptor exactly once
 * per pass, on every shard count, and that passes stay isolated from each other.
 */

#include "broadcast_pool.h"

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#define FAKE_CONNECTIONS 1000

/** Record which shard claimed each descriptor, and how often. */
typedef struct {
    int fds[FAKE_CONNECTIONS];
    int claimed_by[FAKE_CONNECTIONS];   /**< shard index, or -1 */
    int claim_count[FAKE_CONNECTIONS];
    int shard_calls[BROADCAST_POOL_MAX_SHARDS];
} Coverage;

/** Claim this shard's descriptors, exactly as a broadcast task does. */
static void coverage_shard(void* ctx, int shard, int shard_count) {
    Coverage* c = ctx;

    assert(shard >= 0 && shard < shard_count);
    c->shard_calls[shard]++;

    for (int i = 0; i < FAKE_CONNECTIONS; i++) {
        if (shard_count > 1 && (c->fds[i] % shard_count) != shard) continue;

        // No lock: the sharding rule is what makes this safe, and a data race
        // here would show up as a wrong count below (and under TSan).
        c->claimed_by[i] = shard;
        c->claim_count[i]++;
    }
}

/** Verify one pass covered every descriptor once. */
static void check_coverage(Coverage* c, int shard_count) {
    int unclaimed = 0, duplicated = 0, misfiled = 0;

    for (int i = 0; i < FAKE_CONNECTIONS; i++) {
        if (c->claim_count[i] == 0) unclaimed++;
        else if (c->claim_count[i] > 1) duplicated++;

        if (c->claim_count[i] == 1 && shard_count > 1 &&
            c->claimed_by[i] != c->fds[i] % shard_count) {
            misfiled++;
        }
    }

    printf("  %d shards: unclaimed=%d duplicated=%d misfiled=%d\n",
           shard_count, unclaimed, duplicated, misfiled);
    assert(unclaimed == 0);
    assert(duplicated == 0);
    assert(misfiled == 0);
}

static void reset(Coverage* c) {
    memset(c->claim_count, 0, sizeof(c->claim_count));
    memset(c->shard_calls, 0, sizeof(c->shard_calls));
    for (int i = 0; i < FAKE_CONNECTIONS; i++) c->claimed_by[i] = -1;
}

int main(void) {
    printf("=== broadcast fan-out pool ===\n");

    static Coverage cov;
    // Descriptors are not dense in practice; leave gaps so the modulus is
    // exercised against something other than 0..N-1.
    for (int i = 0; i < FAKE_CONNECTIONS; i++) cov.fds[i] = 7 + i * 3;

    /* ------------------------------------------------------------------ */
    printf("\nTEST 1: every descriptor is served exactly once, at any shard count\n");
    for (int shards = 1; shards <= BROADCAST_POOL_MAX_SHARDS; shards++) {
        reset(&cov);
        assert(broadcast_pool_start(shards));
        int active = broadcast_pool_shards();

        broadcast_pool_run(coverage_shard, &cov);
        check_coverage(&cov, active);

        for (int s = 0; s < active; s++) {
            // Every shard ran, and ran once.
            assert(cov.shard_calls[s] == 1);
        }
        broadcast_pool_stop();
    }

    /* ------------------------------------------------------------------ */
    printf("\nTEST 2: repeated passes stay isolated\n");
    {
        assert(broadcast_pool_start(BROADCAST_POOL_MAX_SHARDS));
        int active = broadcast_pool_shards();

        /* A pass must be finished when broadcast_pool_run() returns. If a
         * worker were still running its shard, the next pass would see counts
         * from the previous one and this would report duplicates. */
        for (int pass = 0; pass < 500; pass++) {
            reset(&cov);
            broadcast_pool_run(coverage_shard, &cov);

            for (int i = 0; i < FAKE_CONNECTIONS; i++) assert(cov.claim_count[i] == 1);
        }
        printf("  500 back-to-back passes each covered %d descriptors once (%d shards)\n",
               FAKE_CONNECTIONS, active);
        broadcast_pool_stop();
    }

    /* ------------------------------------------------------------------ */
    printf("\nTEST 3: a pool that never started still runs the work\n");
    {
        /* Callers must not have to check. If the pool failed to start, the pass
         * has to happen anyway — serially — rather than silently not happen. */
        reset(&cov);
        broadcast_pool_run(coverage_shard, &cov);
        for (int i = 0; i < FAKE_CONNECTIONS; i++) assert(cov.claim_count[i] == 1);
        printf("  work ran on the calling thread; shards report %d\n",
               broadcast_pool_shards());
        assert(broadcast_pool_shards() == 1);
    }

    /* ------------------------------------------------------------------ */
    printf("\nTEST 4: bad arguments are handled\n");
    {
        assert(broadcast_pool_start(0));
        printf("  zero shards clamps to %d\n", broadcast_pool_shards());
        assert(broadcast_pool_shards() == 1);
        broadcast_pool_stop();

        assert(broadcast_pool_start(BROADCAST_POOL_MAX_SHARDS * 10));
        printf("  an oversized request clamps to %d\n", broadcast_pool_shards());
        assert(broadcast_pool_shards() == BROADCAST_POOL_MAX_SHARDS);

        broadcast_pool_run(NULL, &cov);      // must not crash
        printf("  a NULL pass function is ignored\n");

        broadcast_pool_stop();
        broadcast_pool_stop();               // idempotent
        printf("  stopping twice is safe\n");
    }

    printf("\nALL ASSERTIONS PASSED\n");
    return 0;
}
