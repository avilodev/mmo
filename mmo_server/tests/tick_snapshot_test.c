/**
 * @file
 * Check gameplay player snapshots, interest queries, slot reuse, and density scaling.
 */

#include "types.h"
#include "log.h"
#include "player_data.h"
#include "players_database.h"
#include "ability_def.h"
#include "quest_system.h"
#include "tick_snapshot.h"
#include "utils.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

extern ActivePlayer active_players[MAX_PLAYERS];

/** Stub successful database initialization. */
int character_database_init(const char* c) { (void)c; return 1; }
/** Stub database shutdown. */
void character_database_close(void) {}
/** Stub successful character persistence. */
int character_update_full_data(const CharacterInfo* d) { (void)d; return 1; }

/** Stub loading empty inventory and equipment arrays. */
int character_items_load(uint32_t c, ItemInstance* inv, int n_inv,
                         ItemInstance* eq, int n_eq) {
    (void)c;
    if (inv) memset(inv, 0, (size_t)n_inv * sizeof(*inv));
    if (eq)  memset(eq,  0, (size_t)n_eq  * sizeof(*eq));
    return 1;
}
/** Stub successful inventory persistence. */
int character_items_save(uint32_t c, const ItemInstance* inv, int n_inv,
                         const ItemInstance* eq, int n_eq) {
    (void)c; (void)inv; (void)n_inv; (void)eq; (void)n_eq; return 1;
}
/** Stub an empty persisted instance-id range. */
uint64_t character_items_max_instance_id(void) { return 0; }

/** Stub a populated level-one character record. */
int character_get_full_data(uint32_t character_id, CharacterInfo* out) {
    memset(out, 0, sizeof(*out));
    out->character_id = character_id;
    snprintf(out->name, sizeof(out->name), "char%u", character_id);
    out->level = 1; out->health = 100; out->max_health = 100;
    out->resource = 50; out->max_resource = 50;
    return 1;
}

/** Stub class-stat application. */
void player_apply_class_stats(ActivePlayer* p) { (void)p; }
/** Stub the stat recompute. */
void player_recompute_stats(ActivePlayer* p) { (void)p; }
/** Stub the hotbar rebuild. */
void ability_refresh_hotbars(ActivePlayer* p) { (void)p; }
/** Stub equipment-stat application. */
void player_apply_equipment_bonuses(ActivePlayer* p) { (void)p; }
/** Stub an empty class ability list. */
int  ability_get_class_abilities(uint8_t c, uint16_t* o, int m) { (void)c;(void)o;(void)m; return 0; }
/** Stub an absent ability definition. */
const AbilityDef* ability_get(uint16_t id) { (void)id; return NULL; }
/** Stub an empty persisted quest list. */
int  quest_player_load(uint32_t c, PlayerQuestEntry* q, int m) { (void)c;(void)q;(void)m; return 0; }
/** Stub successful quest persistence. */
int  quest_player_save(uint32_t c, const PlayerQuestEntry* q, int n) { (void)c;(void)q;(void)n; return 1; }
/** Stub successful packet transmission. */
ssize_t server_send(int fd, void* d, size_t n) { (void)fd;(void)d;(void)n; return (ssize_t)n; }

/** Return the synthetic collision-map extent used by the snapshot grid. */
void world_collision_extent(float* w, float* h) { *w = 8192.0f; *h = 8192.0f; }

static double now_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/** Activate a character and assign its fixture position directly. */
static int spawn_at(uint32_t character_id, int fd, float x, float y) {
    int slot = -1;
    if (!player_add_active(character_id, fd, &slot)) return -1;
    ActivePlayer* p = player_acquire_slot(slot, character_id);
    assert(p != NULL);
    p->pos_x = x;
    p->pos_y = y;
    player_release(p);
    return slot;
}

static int brute_force_count(const TickSnapshot* s, float x, float y, float r) {
    int n = 0;
    for (int i = 0; i < s->count; i++) {
        float dx = s->pos_x[i] - x, dy = s->pos_y[i] - y;
        if (sqrtf(dx * dx + dy * dy) <= r) n++;
    }
    return n;
}

/**
 * Run snapshot correctness, ordering, recycling, and scaling assertions.
 *
 * @return      Zero after all assertions pass.
 */
int main(void) {
    log_init();
    log_set_level(LOG_LEVEL_ERROR);
    assert(playerdata_init("stub") == 1);

    static TickSnapshot snap;
    assert(tick_snapshot_init(&snap) == 1);

    printf("TEST 1: the snapshot mirrors who is online and where\n");
    enum { N = 8 };
    for (int i = 0; i < N; i++)
        assert(spawn_at((uint32_t)(500 + i), 40 + i, 100.0f * (float)i, 200.0f) >= 0);

    tick_snapshot_build(&snap);
    assert(snap.count == N);
    for (int i = 0; i < N; i++) {
        int d = tick_snapshot_find(&snap, (uint32_t)(500 + i));
        assert(d >= 0 && d < snap.count);
        assert(snap.character_id[d] == (uint32_t)(500 + i));
        assert(snap.pos_y[d] == 200.0f);
    }
    assert(tick_snapshot_find(&snap, 999999) == -1);
    printf("  %d players snapshotted; all findable by id, unknown id rejected\n", N);

    printf("\nTEST 2: queries agree with brute force\n");
    const float radii[] = { 50.0f, 150.0f, 400.0f, 1200.0f, 100000.0f };
    for (unsigned r = 0; r < sizeof(radii) / sizeof(radii[0]); r++) {
        for (int i = 0; i < snap.count; i++) {
            int hits[MAX_PLAYERS];
            int got = tick_snapshot_query(&snap, snap.pos_x[i], snap.pos_y[i],
                                          radii[r], hits, MAX_PLAYERS);
            int expect = brute_force_count(&snap, snap.pos_x[i], snap.pos_y[i], radii[r]);
            assert(got == expect);
        }
    }
    printf("  5 radii x %d origins: grid and brute force agree exactly\n", snap.count);

    printf("\nTEST 3: results are nearest-first\n");
    // require nearest-first target candidates
    int hits[MAX_PLAYERS];
    int got = tick_snapshot_query(&snap, 0.0f, 200.0f, 100000.0f, hits, MAX_PLAYERS);
    assert(got == snap.count);
    float prev = -1.0f;
    for (int k = 0; k < got; k++) {
        float dx = snap.pos_x[hits[k]] - 0.0f, dy = snap.pos_y[hits[k]] - 200.0f;
        float d = sqrtf(dx * dx + dy * dy);
        assert(d >= prev - 0.001f);
        prev = d;
    }
    printf("  %d results returned in non-decreasing distance order\n", got);

    // compare every capped prefix to the uncapped ordering
    for (int cap = 1; cap <= snap.count; cap++) {
        int capped[MAX_PLAYERS];
        int n_capped = tick_snapshot_query(&snap, 0.0f, 200.0f, 100000.0f, capped, cap);
        assert(n_capped == cap);
        for (int k = 0; k < n_capped; k++)
            assert(capped[k] == hits[k]);   // same prefix the uncapped query gave
    }
    printf("  capped queries return the same nearest prefix for every cap 1..%d\n", snap.count);

    printf("\nTEST 4: a logged-out player leaves the snapshot\n");
    assert(player_remove_active_if_fd(500, 40) == 1);
    tick_snapshot_build(&snap);
    assert(snap.count == N - 1);
    assert(tick_snapshot_find(&snap, 500) == -1);
    printf("  count %d -> %d, departed character no longer resolves\n", N, snap.count);

    printf("\nTEST 5: a recycled slot does not resurrect the old character\n");
    // recycle a slot without rebuilding the prior snapshot
    int reused = spawn_at(31337, 99, 5000.0f, 5000.0f);
    assert(reused >= 0);
    // Deliberately not rebuilt: the snapshot still describes the previous tick.
    assert(tick_snapshot_find(&snap, 31337) == -1);   // not in this snapshot yet
    assert(tick_snapshot_find(&snap, 500) == -1);     // and the old one stays gone
    tick_snapshot_build(&snap);
    int d = tick_snapshot_find(&snap, 31337);
    assert(d >= 0 && snap.character_id[d] == 31337);
    printf("  stale snapshot resolves neither character; after rebuild only the new one\n");

    printf("\nTEST 6: query cost tracks local density, not player count\n");
    // compare constant-density and fixed-area population growth
    enum { NPCS = 256, REPS = 400, TRIALS = 5, AGGRO_CANDIDATES = 16 };

    struct { int pop; float side; double grid_ms, brute_ms; } run[3];
    // use equal density first, then tenfold local density
    run[0] = (typeof(run[0])){  100, 1897.0f, 0, 0 };   // density d
    run[1] = (typeof(run[1])){ 1000, 6000.0f, 0, 0 };   // density d  (10x pop, 10x area)
    run[2] = (typeof(run[2])){ 1000, 1897.0f, 0, 0 };   // density 10d (10x pop, same area)

    for (int r = 0; r < 3; r++) {
        // Rebuild the population from scratch so each run is independent.
        int slots[MAX_PLAYERS];
        int n_online = player_active_slots(slots, MAX_PLAYERS);
        for (int i = 0; i < n_online; i++) {
            ActivePlayer* p = player_acquire_slot(slots[i], active_players[slots[i]].character_id);
            if (!p) continue;
            uint32_t cid = p->character_id;
            int fd = p->client_fd;
            player_release(p);
            player_remove_active_if_fd(cid, fd);
        }
        assert(player_active_count() == 0);

        float side = run[r].side;
        for (int i = 0; i < run[r].pop; i++) {
            uint32_t hx = (uint32_t)i * 2654435761u;
            uint32_t hy = ((uint32_t)i ^ 0x9E3779B9u) * 2246822519u;
            float x = (float)((hx >> 8) % (uint32_t)side);
            float y = (float)((hy >> 8) % (uint32_t)side);
            assert(spawn_at(300000 + (uint32_t)i, 30000 + i, x, y) >= 0);
        }
        tick_snapshot_build(&snap);
        assert(snap.count == run[r].pop);

        int cand[AGGRO_CANDIDATES];
        long hits_seen = 0;
        double best_grid = 1e18, best_brute = 1e18;

        // retain the least scheduler-affected trial
        for (int trial = 0; trial < TRIALS; trial++) {
            double t = now_seconds();
            for (int rep = 0; rep < REPS; rep++)
                for (int n = 0; n < NPCS; n++) {
                    float nx = fmodf((float)(n * 23), side), ny = fmodf((float)(n * 57), side);
                    hits_seen += tick_snapshot_query(&snap, nx, ny, 300.0f,
                                                     cand, AGGRO_CANDIDATES);
                }
            double ms = (now_seconds() - t) * 1000.0 / REPS;
            if (ms < best_grid) best_grid = ms;

            t = now_seconds();
            volatile long brute = 0;
            for (int rep = 0; rep < REPS; rep++)
                for (int n = 0; n < NPCS; n++) {
                    float nx = fmodf((float)(n * 23), side), ny = fmodf((float)(n * 57), side);
                    for (int i = 0; i < snap.count; i++) {
                        float dx = snap.pos_x[i] - nx, dy = snap.pos_y[i] - ny;
                        if (sqrtf(dx * dx + dy * dy) <= 300.0f) brute++;
                    }
                }
            ms = (now_seconds() - t) * 1000.0 / REPS;
            if (ms < best_brute) best_brute = ms;
        }
        run[r].grid_ms  = best_grid;
        run[r].brute_ms = best_brute;
        assert(hits_seen > 0);   // a query that finds nobody proves nothing
    }

    printf("  %d NPCs querying, milliseconds per tick:\n", NPCS);
    for (int r = 0; r < 3; r++)
        printf("    %4d players over %.0fpx : grid %6.3f | brute %6.3f  (%.1fx)\n",
               run[r].pop, run[r].side, run[r].grid_ms, run[r].brute_ms,
               run[r].brute_ms / (run[r].grid_ms > 1e-6 ? run[r].grid_ms : 1e-6));

    // compare tenfold population at constant density
    double grid_density_growth  = run[1].grid_ms  / run[0].grid_ms;
    double brute_pop_growth     = run[1].brute_ms / run[0].brute_ms;
    printf("    constant density, 10x population -> grid x%.2f, brute x%.2f\n",
           grid_density_growth, brute_pop_growth);
    assert(grid_density_growth < 2.5);
    assert(brute_pop_growth    > 5.0);

    // require the grid to beat brute force at tenfold density
    printf("    10x density on fixed ground  -> grid x%.2f vs its own baseline\n",
           run[2].grid_ms / run[0].grid_ms);
    assert(run[2].grid_ms < run[2].brute_ms);
    printf("  -> cost tracks local density, not population\n");

    printf("\nALL ASSERTIONS PASSED\n");
    tick_snapshot_free(&snap);
    return 0;
}
