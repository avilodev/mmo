/**
 * @file
 * Exercise NPC pool allocation, the identifier index, slot recycling, and the
 * per-tick snapshot the gameplay phases query instead of scanning.
 */

#include "npc_snapshot.h"
#include "npc_world.h"

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** Stand in for the loaded collision map so the snapshot can size its grid.
 *
 * npc_snapshot_init() asks the world for its extent. Linking the real
 * world_collision.c would drag in a world.dat this test has no business
 * needing, so the extent is supplied directly.
 */
void world_collision_extent(float* out_width, float* out_height) {
    if (out_width)  *out_width  = 9600.0f;
    if (out_height) *out_height = 6400.0f;
}

static uint32_t spawn_at(NPCWorld* world, const char* name, float x, float y) {
    return npc_world_spawn(world, name, x, y,
                           /* health */ 100, /* hitbox_radius */ 20.0f,
                           /* dialogue_id */ 0, /* is_interactable */ 0,
                           /* npc_type_id */ 1, /* respawn_time */ 0.0f,
                           NPC_CATEGORY_HOSTILE);
}

/** Report whether a dense snapshot index appears in a query result. */
static int result_contains(const int* results, int count, int dense) {
    for (int i = 0; i < count; i++) if (results[i] == dense) return 1;
    return 0;
}

/* --- concurrency fixture ------------------------------------------------- */

typedef struct {
    NPCWorld* world;
    uint32_t* ids;
    int       id_count;
    int       iterations;
} Hammer;

/** Damage NPCs by identifier, the way a packet thread does. */
static void* damage_thread(void* arg) {
    Hammer* h = arg;
    for (int i = 0; i < h->iterations; i++) {
        uint32_t id = h->ids[i % h->id_count];
        NPCEntity* npc = npc_world_acquire(h->world, id);
        if (npc) {
            npc->health += 1;             // read-modify-write under the slot lock
            npc->pos_x += 0.001f;
            npc_world_release(h->world, npc);
        }
    }
    return NULL;
}

/** Rebuild the tick snapshot, the way the gameplay thread does. */
static void* snapshot_thread(void* arg) {
    Hammer* h = arg;
    NPCTickSnapshot snap;
    assert(npc_snapshot_init(&snap, h->world));

    for (int i = 0; i < h->iterations; i++) {
        npc_snapshot_build(&snap, h->world);
        int out[64];
        (void)npc_snapshot_query(&snap, 500.0f, 500.0f, 4000.0f, out, 64);
    }

    npc_snapshot_free(&snap);
    return NULL;
}

/** Spawn and remove, the way content and respawn logic do. */
static void* churn_thread(void* arg) {
    Hammer* h = arg;
    for (int i = 0; i < h->iterations; i++) {
        uint32_t id = spawn_at(h->world, "churn", 100.0f, 100.0f);
        if (id) npc_world_remove(h->world, id);
    }
    return NULL;
}

int main(void) {
    printf("=== NPC pool and tick snapshot ===\n");

    /* ------------------------------------------------------------------ */
    printf("\nTEST 1: capacity comes from the caller, not a compiled ceiling\n");
    {
        NPCWorld world;
        assert(npc_world_init(&world, 1000));
        printf("  requested 1000, got %d\n", npc_world_capacity(&world));
        assert(npc_world_capacity(&world) == 1000);
        npc_world_shutdown(&world);

        assert(npc_world_init(&world, 0));
        printf("  zero falls back to the default: %d\n", npc_world_capacity(&world));
        assert(npc_world_capacity(&world) == NPC_CAPACITY_DEFAULT);
        npc_world_shutdown(&world);

        assert(npc_world_init(&world, NPC_CAPACITY_MAX * 4));
        printf("  an absurd request is clamped to %d\n", npc_world_capacity(&world));
        assert(npc_world_capacity(&world) == NPC_CAPACITY_MAX);
        npc_world_shutdown(&world);
        printf("  a shut-down pool reports no capacity\n");
        assert(npc_world_capacity(&world) == 0);
    }

    /* ------------------------------------------------------------------ */
    printf("\nTEST 2: spawn, look up by identifier, remove\n");
    {
        NPCWorld world;
        assert(npc_world_init(&world, 64));

        uint32_t a = spawn_at(&world, "alpha", 100.0f, 200.0f);
        uint32_t b = spawn_at(&world, "beta",  300.0f, 400.0f);
        assert(a != 0 && b != 0 && a != b);
        printf("  two spawns produced distinct ids %u and %u, count=%d\n",
               a, b, npc_world_count(&world));
        assert(npc_world_count(&world) == 2);

        NPCEntity* found = npc_world_acquire(&world, b);
        assert(found != NULL);
        printf("  id %u resolves to '%s' at (%.0f, %.0f)\n",
               b, found->name, found->pos_x, found->pos_y);
        assert(strcmp(found->name, "beta") == 0);
        assert(found->pos_x == 300.0f && found->pos_y == 400.0f);
        npc_world_release(&world, found);

        assert(npc_world_acquire(&world, 999999) == NULL);
        assert(npc_world_acquire(&world, 0) == NULL);
        printf("  unknown and zero identifiers resolve to nothing\n");

        npc_world_remove(&world, a);
        assert(npc_world_acquire(&world, a) == NULL);

        NPCEntity* survivor = npc_world_acquire(&world, b);
        assert(survivor != NULL);
        npc_world_release(&world, survivor);   // release before anything else locks

        printf("  removing %u left %u reachable, count=%d\n",
               a, b, npc_world_count(&world));
        assert(npc_world_count(&world) == 1);

        npc_world_shutdown(&world);
    }

    /* ------------------------------------------------------------------ */
    printf("\nTEST 3: a recycled slot is not mistaken for its previous occupant\n");
    {
        /* This is the invariant the tick snapshot depends on. A phase records a
         * slot, the NPC in it dies and is replaced, and a later phase in the
         * same tick reaches that slot. Without the identifier check the damage
         * lands on whatever moved in. */
        NPCWorld world;
        assert(npc_world_init(&world, 4));

        uint32_t original = spawn_at(&world, "original", 10.0f, 10.0f);
        NPCEntity* npc = npc_world_acquire(&world, original);
        assert(npc != NULL);
        int slot = 0;
        for (int i = 0; i < npc_world_capacity(&world); i++)
            if (npc_world_slot(&world, i) == npc) slot = i;
        npc->is_alive = 0;                     // die, leaving the slot reclaimable
        npc_world_release(&world, npc);

        // Fill the pool so the reclaim lands on the dead NPC's slot.
        uint32_t replacement = 0;
        for (int i = 0; i < 8 && replacement == 0; i++) {
            uint32_t id = spawn_at(&world, "replacement", 20.0f, 20.0f);
            NPCEntity* candidate = npc_world_acquire(&world, id);
            if (candidate && npc_world_slot(&world, slot) == candidate) replacement = id;
            if (candidate) npc_world_release(&world, candidate);
        }
        assert(replacement != 0 && replacement != original);
        printf("  slot %d now holds %u, not %u\n", slot, replacement, original);

        assert(npc_world_acquire_slot(&world, slot, original) == NULL);
        printf("  acquiring that slot with the stale id is refused\n");

        NPCEntity* fresh = npc_world_acquire_slot(&world, slot, replacement);
        assert(fresh != NULL);
        printf("  acquiring it with the current id succeeds\n");
        npc_world_release(&world, fresh);

        assert(npc_world_acquire_slot(&world, -1, replacement) == NULL);
        assert(npc_world_acquire_slot(&world, 9999, replacement) == NULL);
        printf("  out-of-range slots are refused\n");

        npc_world_shutdown(&world);
    }

    /* ------------------------------------------------------------------ */
    printf("\nTEST 4: the identifier index survives heavy reuse\n");
    {
        /* Removal tombstones a bucket rather than emptying it. Get that wrong
         * and lookups start missing entries that probed past the hole. */
        NPCWorld world;
        assert(npc_world_init(&world, 32));

        for (int cycle = 0; cycle < 200; cycle++) {
            uint32_t ids[16];
            for (int i = 0; i < 16; i++) {
                ids[i] = spawn_at(&world, "cycle", (float)i * 10.0f, 0.0f);
                assert(ids[i] != 0);
            }
            for (int i = 0; i < 16; i++) {
                NPCEntity* e = npc_world_acquire(&world, ids[i]);
                assert(e != NULL);              // every id still reachable
                npc_world_release(&world, e);
            }
            for (int i = 0; i < 16; i++) npc_world_remove(&world, ids[i]);
            for (int i = 0; i < 16; i++) assert(npc_world_acquire(&world, ids[i]) == NULL);
        }
        printf("  200 cycles of 16 spawn/lookup/remove stayed consistent\n");
        printf("  pool is empty afterwards: count=%d\n", npc_world_count(&world));
        assert(npc_world_count(&world) == 0);

        npc_world_shutdown(&world);
    }

    /* ------------------------------------------------------------------ */
    printf("\nTEST 5: a full pool refuses rather than overruns\n");
    {
        NPCWorld world;
        assert(npc_world_init(&world, 8));
        for (int i = 0; i < 8; i++) assert(spawn_at(&world, "full", 0.0f, 0.0f) != 0);
        printf("  8 of 8 slots filled, count=%d\n", npc_world_count(&world));
        assert(npc_world_count(&world) == 8);

        uint32_t overflow = spawn_at(&world, "overflow", 0.0f, 0.0f);
        printf("  the ninth spawn returns %u (expect 0)\n", overflow);
        assert(overflow == 0);
        npc_world_shutdown(&world);
    }

    /* ------------------------------------------------------------------ */
    printf("\nTEST 6: the snapshot holds living NPCs only\n");
    {
        NPCWorld world;
        assert(npc_world_init(&world, 32));

        uint32_t alive = spawn_at(&world, "alive", 1000.0f, 1000.0f);
        uint32_t dead  = spawn_at(&world, "dead",  1010.0f, 1000.0f);

        NPCEntity* corpse = npc_world_acquire(&world, dead);
        corpse->is_alive = 0;
        npc_world_release(&world, corpse);

        NPCTickSnapshot snap;
        assert(npc_snapshot_init(&snap, &world));
        npc_snapshot_build(&snap, &world);

        printf("  2 spawned, 1 dead, snapshot count=%d\n", snap.count);
        assert(snap.count == 1);
        assert(snap.id[0] == alive);
        assert(snap.pos_x[0] == 1000.0f);
        printf("  the surviving entry is id %u at (%.0f, %.0f)\n",
               snap.id[0], snap.pos_x[0], snap.pos_y[0]);

        printf("  largest hitbox recorded: %.0f\n", snap.max_hitbox_radius);
        assert(snap.max_hitbox_radius == 20.0f);

        npc_snapshot_free(&snap);
        npc_world_shutdown(&world);
    }

    /* ------------------------------------------------------------------ */
    printf("\nTEST 7: snapshot queries match a brute-force sweep\n");
    {
        NPCWorld world;
        assert(npc_world_init(&world, 400));

        srand(20240607);
        for (int i = 0; i < 400; i++) {
            float x = (float)(rand() % 9600);
            float y = (float)(rand() % 6400);
            assert(spawn_at(&world, "scatter", x, y) != 0);
        }

        NPCTickSnapshot snap;
        assert(npc_snapshot_init(&snap, &world));
        npc_snapshot_build(&snap, &world);
        assert(snap.count == 400);

        int mismatches = 0, total_hits = 0;
        for (int q = 0; q < 300; q++) {
            float qx = (float)(rand() % 9600);
            float qy = (float)(rand() % 6400);
            float radius = (float)(50 + rand() % 1500);

            int results[400];
            int found = npc_snapshot_query(&snap, qx, qy, radius, results, 400);

            int expected = 0;
            for (int d = 0; d < snap.count; d++) {
                float dx = snap.pos_x[d] - qx;
                float dy = snap.pos_y[d] - qy;
                if (dx * dx + dy * dy <= radius * radius) {
                    expected++;
                    if (!result_contains(results, found, d)) mismatches++;
                }
            }
            if (found != expected) mismatches++;
            total_hits += found;
        }
        printf("  300 queries, %d hits total, %d mismatches vs brute force\n",
               total_hits, mismatches);
        assert(mismatches == 0);

        npc_snapshot_free(&snap);
        npc_world_shutdown(&world);
    }

    /* ------------------------------------------------------------------ */
    printf("\nTEST 8: a snapshot slot resolves back to the same NPC\n");
    {
        NPCWorld world;
        assert(npc_world_init(&world, 16));
        for (int i = 0; i < 10; i++)
            assert(spawn_at(&world, "resolve", (float)(i * 100), 500.0f) != 0);

        NPCTickSnapshot snap;
        assert(npc_snapshot_init(&snap, &world));
        npc_snapshot_build(&snap, &world);

        int resolved = 0;
        for (int d = 0; d < snap.count; d++) {
            NPCEntity* npc = npc_world_acquire_slot(&world, snap.slot[d], snap.id[d]);
            assert(npc != NULL);
            assert(npc->id == snap.id[d]);
            assert(npc->pos_x == snap.pos_x[d]);
            npc_world_release(&world, npc);
            resolved++;
        }
        printf("  all %d snapshot entries resolved to their live entity\n", resolved);
        assert(resolved == 10);

        npc_snapshot_free(&snap);
        npc_world_shutdown(&world);
    }

    /* ------------------------------------------------------------------ */
    printf("\nTEST 9: readers, writers, and churn run concurrently\n");
    {
        /* The whole point of splitting the lock is that these three do not
         * exclude each other. Correctness under contention is checked here;
         * `make npc-world-sanitize` runs the same thing under ThreadSanitizer. */
        NPCWorld world;
        assert(npc_world_init(&world, 256));

        enum { RESIDENTS = 100 };
        uint32_t ids[RESIDENTS];
        for (int i = 0; i < RESIDENTS; i++) {
            ids[i] = spawn_at(&world, "resident",
                              (float)(rand() % 4000), (float)(rand() % 4000));
            assert(ids[i] != 0);
        }

        Hammer h = { .world = &world, .ids = ids,
                     .id_count = RESIDENTS, .iterations = 2000 };

        pthread_t damagers[4], snapshots[2], churners[2];
        for (int i = 0; i < 4; i++) assert(pthread_create(&damagers[i], NULL, damage_thread, &h) == 0);
        for (int i = 0; i < 2; i++) assert(pthread_create(&snapshots[i], NULL, snapshot_thread, &h) == 0);
        for (int i = 0; i < 2; i++) assert(pthread_create(&churners[i], NULL, churn_thread, &h) == 0);

        for (int i = 0; i < 4; i++) pthread_join(damagers[i], NULL);
        for (int i = 0; i < 2; i++) pthread_join(snapshots[i], NULL);
        for (int i = 0; i < 2; i++) pthread_join(churners[i], NULL);

        printf("  4 damagers, 2 snapshotters, 2 churners finished\n");

        /* Every resident took exactly (4 threads * 2000 / 100) increments.
         * A lost update here would mean the slot lock is not actually
         * serialising the read-modify-write. */
        const int expected_health = 100 + (4 * 2000) / RESIDENTS;
        int wrong = 0;
        for (int i = 0; i < RESIDENTS; i++) {
            NPCEntity* npc = npc_world_acquire(&world, ids[i]);
            assert(npc != NULL);
            if (npc->health != expected_health) wrong++;
            npc_world_release(&world, npc);
        }
        printf("  every resident reached health %d: %d wrong\n", expected_health, wrong);
        assert(wrong == 0);

        npc_world_shutdown(&world);
    }

    printf("\nALL ASSERTIONS PASSED\n");
    return 0;
}
