/**
 * @file
 * Check active-player indexing, slot recycling, lookup scaling, and writer progress.
 */

#include "types.h"
#include "log.h"
#include "player_data.h"
#include "players_database.h"
#include "ability_def.h"
#include "quest_system.h"
#include "utils.h"

#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

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
/** Stub the whole-character save that commits scalars, currency and items together. */
int character_save_all(const CharacterInfo* d, const ItemInstance* inv, int n_inv,
                       const ItemInstance* eq, int n_eq) {
    (void)d; (void)inv; (void)n_inv; (void)eq; (void)n_eq; return 1;
}
/** Stub recording that a character is live in a world. */
int world_session_mark(uint32_t character_id, uint32_t world_id) {
    (void)character_id; (void)world_id; return 1;
}
/** Stub clearing that record. */
void world_session_clear(uint32_t character_id) { (void)character_id; }
/** Stub an empty persisted instance-id range. */
uint64_t character_items_max_instance_id(void) { return 0; }

/** Stub a populated level-one character record. */
int character_get_full_data(uint32_t character_id, CharacterInfo* out) {
    memset(out, 0, sizeof(*out));
    out->character_id = character_id;
    snprintf(out->name, sizeof(out->name), "char%u", character_id);
    out->level = 1; out->health = 100; out->max_health = 100;
    out->resource = 50; out->max_resource = 50; out->pos_x = 100.0f; out->pos_y = 100.0f;
    return 1;
}

/** Stub class-stat application. */
void player_apply_class_stats(ActivePlayer* p) { (void)p; }
/** Stub equipment-stat application. */
void player_apply_equipment_bonuses(ActivePlayer* p) { (void)p; }
/** Stub an empty hotbar for both forms. */
int  ability_get_form_abilities(uint8_t race, uint8_t form, uint16_t* out, int max) {
    (void)race; (void)form; (void)out; (void)max; return 0;
}
/** Stub the hotbar rebuild. */
void ability_refresh_hotbars(ActivePlayer* p) { (void)p; }
/** Stub the stat recompute. */
void player_recompute_stats(ActivePlayer* p) { (void)p; }
/** Stub an absent ability definition. */
const AbilityDef* ability_get(uint16_t id) { (void)id; return NULL; }
/** Stub an empty persisted quest list. */
int  quest_player_load(uint32_t c, PlayerQuestState* s) {
    (void)c; (void)s; return 0;
}
/** Stub successful quest persistence. */
int  quest_player_save(uint32_t c, const PlayerQuestState* s) {
    (void)c; (void)s; return 1;
}
/** Stub quest storage teardown; these tests never allocate any. */
void quest_state_release(PlayerQuestState* s) { (void)s; }
/** Stub the snapshot's deep copy; these tests hold no quest state to copy. */
int  quest_state_copy(PlayerQuestState* out, const PlayerQuestState* src) {
    (void)src; if (out) memset(out, 0, sizeof(*out)); return 1;
}
/** Stub the completion set; these tests never record one. */
int  quest_history_add(QuestHistory* h, uint32_t id) { (void)h; (void)id; return 1; }
/** Stub successful packet transmission. */
ssize_t server_send(int fd, void* d, size_t n) { (void)fd; (void)d; (void)n; return (ssize_t)n; }

static double now_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/** Measure average successful lookup latency across an identifier set. */
static double time_per_lookup_ns(const uint32_t* ids, int id_count, int iterations) {
    double start = now_seconds();
    int found = 0;
    for (int it = 0; it < iterations; it++) {
        for (int i = 0; i < id_count; i++) {
            ActivePlayer* p = player_acquire(ids[i]);
            if (p) { found++; player_release(p); }
        }
    }
    double elapsed = now_seconds() - start;
    assert(found == id_count * iterations);
    return (elapsed * 1e9) / (double)(id_count * iterations);
}

static atomic_int g_readers_run = 1;

/** Repeatedly scan the active-slot list under the registry read lock. */
static void* hammer_reader(void* a) {
    (void)a;
    volatile int sink = 0;
    while (atomic_load(&g_readers_run)) {
        player_registry_rdlock();
        int count = 0;
        const int* online = player_active_list_locked(&count);
        for (int i = 0; i < count; i++) sink += online[i];
        player_registry_unlock();
    }
    (void)sink;
    return NULL;
}

static atomic_int g_watchdog_armed = 0;

/** Terminate the test if a registry writer remains starved for ten seconds. */
static void* starvation_watchdog(void* a) {
    (void)a;
    for (int i = 0; i < 100; i++) {          // 10s budget, checked every 100ms
        usleep(100000);
        if (!atomic_load(&g_watchdog_armed)) return NULL;
    }
    fprintf(stderr,
            "\nFAILED: a writer could not acquire the registry lock in 10s "
            "while readers were active.\n"
            "The lock is reader-preferring — check that "
            "registry_lock_prefer_writers() is actually compiled in\n"
            "(PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP is an enum, so an "
            "#ifdef guard on it silently disables the fix).\n");
    _exit(1);
}

/** Remove each listed character using its expected descriptor. */
static void logout_all(const uint32_t* ids, int n, const int* fds) {
    for (int i = 0; i < n; i++) {
        int removed = player_remove_active_if_fd(ids[i], fds[i]);
        assert(removed == 1);
    }
}

/**
 * Run active-player index and registry-lock assertions.
 *
 * @return      Zero after all assertions pass.
 */
int main(void) {
    log_init();
    log_set_level(LOG_LEVEL_ERROR);
    assert(playerdata_init("stub") == 1);

    printf("TEST 1: every logged-in character is findable, and by slot\n");
    enum { SMALL = 10 };
    uint32_t small_ids[SMALL];
    int      small_fds[SMALL];
    int      small_slots[SMALL];

    for (int i = 0; i < SMALL; i++) {
        small_ids[i] = (uint32_t)(100 + i);
        small_fds[i] = 10 + i;
        int slot = -1;
        assert(player_add_active(small_ids[i], small_fds[i], &slot) == 1);
        assert(slot >= 0 && slot < MAX_PLAYERS);
        small_slots[i] = slot;
    }

    for (int i = 0; i < SMALL; i++) {
        ActivePlayer* by_id = player_acquire(small_ids[i]);
        assert(by_id != NULL);
        assert(by_id->character_id == small_ids[i]);
        player_release(by_id);

        ActivePlayer* by_slot = player_acquire_slot(small_slots[i], small_ids[i]);
        assert(by_slot != NULL);
        assert(by_slot == by_id);   // same slot, same address
        player_release(by_slot);
    }
    printf("  %d characters, all resolvable by id and by slot\n", SMALL);

    // A character that never logged in must not resolve.
    assert(player_acquire(999999) == NULL);
    printf("  unknown character id returns NULL\n");

    printf("\nTEST 2: the active-slot list matches real occupancy\n");
    int listed[MAX_PLAYERS];
    int n_listed = player_active_slots(listed, MAX_PLAYERS);
    assert(n_listed == SMALL);
    assert(player_active_count() == SMALL);

    for (int i = 0; i < n_listed; i++) {
        assert(listed[i] >= 0 && listed[i] < MAX_PLAYERS);
        assert(active_players[listed[i]].is_loaded);
    }
    // A short buffer must truncate, not overrun.
    int tiny[3];
    assert(player_active_slots(tiny, 3) == 3);
    printf("  list reports %d slots, all occupied; truncation respected\n", n_listed);

    printf("\nTEST 3: ABA guard — a recycled slot never returns the wrong character\n");
    int   victim_slot = small_slots[0];
    uint32_t victim_id = small_ids[0];

    assert(player_remove_active_if_fd(victim_id, small_fds[0]) == 1);
    assert(player_acquire(victim_id) == NULL);
    assert(player_acquire_slot(victim_slot, victim_id) == NULL);
    printf("  after logout: id lookup NULL, slot lookup NULL\n");

    // recycle the lowest free slot with another character
    int    intruder_slot = -1;
    uint32_t intruder_id = 424242;
    assert(player_add_active(intruder_id, 77, &intruder_slot) == 1);
    assert(intruder_slot == victim_slot);

    // reject the stale slot and identifier pair
    assert(player_acquire_slot(victim_slot, victim_id) == NULL);
    // ...while the new occupant resolves normally.
    ActivePlayer* intruder = player_acquire_slot(intruder_slot, intruder_id);
    assert(intruder != NULL && intruder->character_id == intruder_id);
    player_release(intruder);
    printf("  slot recycled to char %u: stale pair refused, new pair works\n", intruder_id);

    assert(player_remove_active_if_fd(intruder_id, 77) == 1);

    printf("\nTEST 4: lookup cost does not grow with the player count\n");
    // Re-establish the small population (slot 0's character was removed above).
    small_slots[0] = -1;
    assert(player_add_active(small_ids[0], small_fds[0], &small_slots[0]) == 1);

    enum { ITER_SMALL = 20000 };
    double ns_small = time_per_lookup_ns(small_ids, SMALL, ITER_SMALL);

    // Fill the table to capacity.
    enum { BIG = MAX_PLAYERS };
    static uint32_t big_ids[BIG];
    static int      big_fds[BIG];
    int added = SMALL;
    for (int i = 0; i < SMALL; i++) { big_ids[i] = small_ids[i]; big_fds[i] = small_fds[i]; }
    for (int i = SMALL; i < BIG; i++) {
        big_ids[i] = (uint32_t)(5000 + i);
        big_fds[i] = 5000 + i;
        int slot = -1;
        if (player_add_active(big_ids[i], big_fds[i], &slot) != 1) break;
        added++;
    }
    assert(added == BIG);
    assert(player_active_count() == BIG);

    enum { ITER_BIG = 200 };
    double ns_big = time_per_lookup_ns(big_ids, BIG, ITER_BIG);

    double ratio = ns_big / ns_small;
    printf("  %4d players: %7.1f ns per acquire\n", SMALL, ns_small);
    printf("  %4d players: %7.1f ns per acquire\n", BIG, ns_big);
    printf("  ratio: %.2fx  (linear scan would be ~%dx)\n", ratio, BIG / SMALL);

    // distinguish indexed lookup from linear scaling
    assert(ratio < 8.0);

    logout_all(big_ids, BIG, big_fds);
    assert(player_active_count() == 0);
    assert(player_active_slots(listed, MAX_PLAYERS) == 0);
    printf("  all %d logged out, index and list drained\n", BIG);

    printf("\nTEST 5: index survives repeated login/logout churn\n");
    // churn tombstones across repeated login cycles
    for (int round = 0; round < 50; round++) {
        enum { C = 40 };
        uint32_t ids[C];
        int      fds[C];
        for (int i = 0; i < C; i++) {
            ids[i] = (uint32_t)(round * 1000 + i + 1);
            fds[i] = 200 + i;
            int slot = -1;
            assert(player_add_active(ids[i], fds[i], &slot) == 1);
        }
        for (int i = 0; i < C; i++) {
            ActivePlayer* p = player_acquire(ids[i]);
            assert(p != NULL && p->character_id == ids[i]);
            player_release(p);
        }
        logout_all(ids, C, fds);
        assert(player_active_count() == 0);
    }
    printf("  50 rounds x 40 characters: every lookup resolved\n");

    printf("\nTEST 6: a login is not starved by continuous readers\n");
    // measure writer progress under continuous readers
    for (int i = 0; i < SMALL; i++) {
        int slot = -1;
        assert(player_add_active(small_ids[i], small_fds[i], &slot) == 1);
    }

    long cores = sysconf(_SC_NPROCESSORS_ONLN);
    int n_readers = (int)(cores > 4 ? cores : 4);
    if (n_readers > 16) n_readers = 16;

    atomic_store(&g_readers_run, 1);
    pthread_t readers[16];
    for (int i = 0; i < n_readers; i++)
        pthread_create(&readers[i], NULL, hammer_reader, NULL);

    usleep(50000);   // let the readers saturate before measuring

    atomic_store(&g_watchdog_armed, 1);
    pthread_t watchdog;
    pthread_create(&watchdog, NULL, starvation_watchdog, NULL);

    double worst_wait_ms = 0.0;
    for (int k = 0; k < 100; k++) {
        double t0 = now_seconds();
        player_registry_wrlock();
        double waited_ms = (now_seconds() - t0) * 1000.0;
        player_registry_unlock();
        if (waited_ms > worst_wait_ms) worst_wait_ms = waited_ms;
        usleep(500);
    }

    atomic_store(&g_watchdog_armed, 0);
    pthread_join(watchdog, NULL);

    atomic_store(&g_readers_run, 0);
    for (int i = 0; i < n_readers; i++) pthread_join(readers[i], NULL);

    printf("  %d reader threads hammering the registry\n", n_readers);
    printf("  worst writer wait: %.2f ms\n", worst_wait_ms);

    // allow scheduling variance while detecting starvation
    assert(worst_wait_ms < 50.0);
    printf("  -> writers are not starved\n");

    printf("\nALL ASSERTIONS PASSED\n");
    return 0;
}
