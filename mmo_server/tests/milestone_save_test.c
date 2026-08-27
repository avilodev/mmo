/**
 * @file
 * Check that state a player would file a ticket about losing does not wait for
 * the two-minute sweep.
 *
 * The periodic save ran every SAVE_INTERVAL_SECONDS and took every player whose
 * is_dirty was set. That is the right cadence for a position and a health bar,
 * and the wrong one for a level gained, a quest turned in, coin spent, or a
 * rare drop picked up -- all of which a crash between sweeps silently rewound.
 *
 * What is under test is the *selection*, not the timer. A milestone pass that
 * ran every five seconds but still picked players by is_dirty would be a second
 * full sweep at 24x the cost and would fix nothing, so every case here is about
 * which players each pass takes and which bits it leaves behind. The timer
 * itself is one assertion at the end: that the two intervals are different, and
 * which way round.
 *
 * The database is stubbed the way login_concurrency_test stubs it. What is
 * being checked is which snapshots reach player_commit_save(), so a stub that
 * records them is the whole apparatus needed -- and a stub that can be made to
 * fail is what exposes the restore path, where losing the critical bit would
 * quietly demote a retry back onto the slow sweep.
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern ActivePlayer active_players[MAX_PLAYERS];

static int passed = 0;

static void check(int condition, const char* what) {
    printf("  %-4s %s\n", condition ? "ok" : "FAIL", what);
    if (!condition) exit(1);
    passed++;
}

/* --- The database, as far as this test is concerned ---------------------- */

/** Every character_save_all() this pass saw, in order. */
#define COMMIT_CAP 512
static uint32_t g_committed[COMMIT_CAP];
static int      g_commit_count = 0;

/** When nonzero, every commit for this character fails. */
static uint32_t g_fail_character = 0;

static void commits_reset(void) { g_commit_count = 0; }

static int committed_contains(uint32_t character_id) {
    for (int i = 0; i < g_commit_count; i++)
        if (g_committed[i] == character_id) return 1;
    return 0;
}

int character_save_all(const CharacterInfo* d, const ItemInstance* inv, int n_inv,
                       const ItemInstance* eq, int n_eq) {
    (void)inv; (void)n_inv; (void)eq; (void)n_eq;
    if (d && d->character_id == g_fail_character) return 0;
    if (d && g_commit_count < COMMIT_CAP) g_committed[g_commit_count++] = d->character_id;
    return 1;
}

/* --- Everything else player_data.c reaches for --------------------------- */

int character_database_init(const char* c) { (void)c; return 1; }
void character_database_close(void) {}
int character_update_full_data(const CharacterInfo* d) { (void)d; return 1; }

int character_items_load(uint32_t c, ItemInstance* inv, int n_inv,
                         ItemInstance* eq, int n_eq) {
    (void)c;
    if (inv) memset(inv, 0, sizeof(ItemInstance) * (size_t)n_inv);
    if (eq)  memset(eq,  0, sizeof(ItemInstance) * (size_t)n_eq);
    return 1;
}
int character_items_save(uint32_t c, const ItemInstance* inv, int n_inv,
                         const ItemInstance* eq, int n_eq) {
    (void)c; (void)inv; (void)n_inv; (void)eq; (void)n_eq; return 1;
}
int world_session_mark(uint32_t character_id, uint32_t world_id) {
    (void)character_id; (void)world_id; return 1;
}
void world_session_clear(uint32_t character_id) { (void)character_id; }
uint64_t character_items_max_instance_id(void) { return 0; }

int character_get_full_data(uint32_t character_id, CharacterInfo* out) {
    if (!out) return 0;
    memset(out, 0, sizeof(*out));
    out->character_id = character_id;
    snprintf(out->name, sizeof(out->name), "char%u", character_id);
    out->level = 1;
    out->pos_x = 100.0f;
    out->pos_y = 100.0f;
    return 1;
}

void player_apply_class_stats(ActivePlayer* p) { (void)p; }
void player_apply_equipment_bonuses(ActivePlayer* p) { (void)p; }
int  ability_get_form_abilities(uint8_t race, uint8_t form, uint16_t* out, int max) {
    (void)race; (void)form; (void)out; (void)max; return 0;
}
void ability_refresh_hotbars(ActivePlayer* p) { (void)p; }
void player_recompute_stats(ActivePlayer* p) { (void)p; }
const AbilityDef* ability_get(uint16_t id) { (void)id; return NULL; }
int  quest_player_load(uint32_t c, PlayerQuestState* s) {
    (void)c; if (s) memset(s, 0, sizeof(*s)); return 1;
}
int  quest_player_save(uint32_t c, const PlayerQuestState* s) { (void)c; (void)s; return 1; }
void quest_state_release(PlayerQuestState* s) { (void)s; }
int  quest_state_copy(PlayerQuestState* out, const PlayerQuestState* src) {
    (void)src; if (out) memset(out, 0, sizeof(*out)); return 1;
}
int  quest_history_add(QuestHistory* h, uint32_t id) { (void)h; (void)id; return 1; }
ssize_t server_send(int fd, void* d, size_t n) { (void)fd; (void)d; (void)n; return (ssize_t)n; }

/* --- Helpers ------------------------------------------------------------- */

/** Log a character in and leave it clean, whatever the load path did. */
static void login(uint32_t character_id, int fd) {
    assert(player_add_active(character_id, fd, NULL) == PLAYER_ADD_READY);
    ActivePlayer* p = player_acquire(character_id);
    assert(p);
    p->is_dirty          = 0;
    p->is_dirty_critical = 0;
    player_release(p);
}

/** Set is_dirty only -- the ordinary "this character moved" change. */
static void mark_ordinary(uint32_t character_id) {
    ActivePlayer* p = player_acquire(character_id);
    assert(p);
    p->is_dirty = 1;
    player_release(p);
}

/** Set the milestone bit through the helper the gameplay paths use. */
static void mark_milestone(uint32_t character_id) {
    ActivePlayer* p = player_acquire(character_id);
    assert(p);
    player_mark_critical(p);
    player_release(p);
}

/** Read back a character's two dirty bits. */
static void dirty_bits(uint32_t character_id, int* dirty, int* critical) {
    ActivePlayer* p = player_acquire(character_id);
    assert(p);
    *dirty    = p->is_dirty;
    *critical = p->is_dirty_critical;
    player_release(p);
}

static void logout_all(void) {
    for (int i = 0; i < MAX_PLAYERS; i++)
        if (active_players[i].is_loaded)
            player_remove_active(active_players[i].character_id);
}

/* --- Tests --------------------------------------------------------------- */

static void test_the_milestone_pass_takes_only_milestones(void) {
    printf("the milestone pass writes milestones and leaves ordinary changes alone\n");

    login(1001, 501);
    login(1002, 502);
    login(1003, 503);

    mark_ordinary(1001);
    mark_milestone(1002);
    /* 1003 changed nothing at all. */

    commits_reset();
    int written = playerdata_flush_saves(1 /* critical_only */);

    check(written == 1, "exactly one character was written");
    check(committed_contains(1002), "the one carrying a milestone");
    check(!committed_contains(1001), "and not the one that merely moved");
    check(!committed_contains(1003), "nor the one that did nothing");

    /* The point of leaving 1001 alone is that it is still owed a write. A pass
     * that cleared is_dirty without saving would lose the move outright. */
    int dirty = 0, critical = 0;
    dirty_bits(1001, &dirty, &critical);
    check(dirty && !critical, "the ordinary change is still pending for the sweep");

    /* And the milestone is settled in both senses. Clearing only is_dirty here
     * would have the milestone pass pick 1002 up again every five seconds for
     * the rest of the session, writing an unchanged row each time. */
    dirty_bits(1002, &dirty, &critical);
    check(!dirty && !critical, "the milestone is clear in both senses");

    logout_all();
}

static void test_the_full_sweep_takes_both(void) {
    printf("the full sweep takes ordinary changes and milestones together\n");

    login(1001, 501);
    login(1002, 502);
    mark_ordinary(1001);
    mark_milestone(1002);

    commits_reset();
    int written = playerdata_flush_saves(0 /* every dirty player */);

    check(written == 2, "both characters were written");
    check(committed_contains(1001) && committed_contains(1002),
          "the mover and the one at a milestone");

    /* is_dirty_critical implies is_dirty, so the sweep needs one test and picks
     * up milestones on the way past. A sweep that tested them separately would
     * either miss one or write it twice. */
    int dirty = 0, critical = 0;
    dirty_bits(1002, &dirty, &critical);
    check(!dirty && !critical, "and the milestone bit was cleared by the sweep too");

    commits_reset();
    check(playerdata_flush_saves(0) == 0, "a second sweep has nothing left to write");

    logout_all();
}

static void test_a_failed_milestone_commit_stays_a_milestone(void) {
    printf("a milestone whose commit fails is retried as a milestone\n");

    login(1001, 501);
    login(1002, 502);
    mark_ordinary(1001);
    mark_milestone(1002);

    /* The database refuses this character. The snapshot does not record which
     * bit the scan cleared, so restoring the right one is the thing that can
     * silently regress: putting back a bare is_dirty would leave the level-up
     * to the two-minute sweep after all, which is the bug this whole pass
     * exists to close -- and no test that only checked "it retries" would
     * notice. */
    g_fail_character = 1002;

    commits_reset();
    check(playerdata_flush_saves(1) == 0, "the failing commit wrote nothing");

    int dirty = 0, critical = 0;
    dirty_bits(1002, &dirty, &critical);
    check(dirty && critical, "and the character is still marked as a milestone");

    /* Which is to say the next milestone pass picks it up -- not the next
     * sweep, two minutes later. */
    g_fail_character = 0;
    commits_reset();
    check(playerdata_flush_saves(1) == 1, "so the next milestone pass retries it");
    check(committed_contains(1002), "and it lands");

    /* 1001 was never touched by any of this. */
    dirty_bits(1001, &dirty, &critical);
    check(dirty && !critical, "the ordinary change was not disturbed");

    logout_all();
}

static void test_a_failed_ordinary_commit_stays_ordinary(void) {
    printf("a failed ordinary commit is not promoted to a milestone\n");

    login(1001, 501);
    mark_ordinary(1001);
    g_fail_character = 1001;

    commits_reset();
    check(playerdata_flush_saves(0) == 0, "the sweep's commit failed");

    int dirty = 0, critical = 0;
    dirty_bits(1001, &dirty, &critical);
    check(dirty && !critical, "and the character is dirty but not critical");

    /* The restore has to put back what the scan cleared, in both directions. A
     * restore that always marked critical would drag every database outage onto
     * the five-second pass and hammer the failing database 24x harder. */
    g_fail_character = 0;
    commits_reset();
    check(playerdata_flush_saves(1) == 0, "so the milestone pass still skips it");
    check(playerdata_flush_saves(0) == 1, "and the next sweep is what retries it");

    logout_all();
}

static void test_more_milestones_than_one_batch(void) {
    printf("more milestones than fit in one batch are all written\n");

    /* SAVE_BATCH_SIZE is 64 and is private to player_data.c; 150 is comfortably
     * past it however it is tuned within reason. The drain restarts its scan
     * from the top of the online list each pass, which is only correct because
     * the bits are cleared under the slot lock as each snapshot is taken --
     * otherwise the first batch would be rescanned and rewritten forever. */
    enum { N = 150 };
    for (int i = 0; i < N; i++) login((uint32_t)(2000 + i), 600 + i);
    for (int i = 0; i < N; i++) mark_milestone((uint32_t)(2000 + i));

    commits_reset();
    int written = playerdata_flush_saves(1);

    check(written == N, "every one of them was written");

    int all_present = 1, duplicated = 0;
    for (int i = 0; i < N; i++)
        if (!committed_contains((uint32_t)(2000 + i))) all_present = 0;
    for (int i = 0; i < g_commit_count; i++)
        for (int j = i + 1; j < g_commit_count; j++)
            if (g_committed[i] == g_committed[j]) duplicated = 1;

    check(all_present, "each character exactly once -- none missed");
    check(!duplicated, "and none written twice");

    commits_reset();
    check(playerdata_flush_saves(1) == 0, "and the pass after has nothing to do");

    logout_all();
}

static void test_logout_settles_both_bits(void) {
    printf("a logout save settles a milestone rather than leaving it pending\n");

    login(1001, 501);
    mark_milestone(1001);

    ActivePlayer* p = player_acquire(1001);
    assert(p);
    commits_reset();
    check(playerdata_save(p) == 1, "the logout path wrote the character");
    int dirty = p->is_dirty, critical = p->is_dirty_critical;
    player_release(p);

    check(!dirty && !critical, "and cleared both bits");

    /* Otherwise the milestone pass would rewrite a character on its way out the
     * door, racing the removal that is already saving it. */
    commits_reset();
    check(playerdata_flush_saves(1) == 0, "so no pass rewrites it afterwards");

    logout_all();
}

static void test_the_two_cadences_differ(void) {
    printf("the milestone pass runs on a shorter timer than the sweep\n");

    int fast = playerdata_critical_interval();
    int slow = playerdata_save_interval();

    printf("       milestone every %ds, full sweep every %ds\n", fast, slow);
    check(fast > 0 && slow > 0, "both intervals are set");
    check(fast < slow, "the milestone pass is the faster of the two");
    check(fast <= 10, "and is fast enough that a crash costs seconds, not minutes");
}

int main(void) {
    log_init();
    log_set_level(LOG_LEVEL_ERROR);   // the failure cases log deliberately
    assert(playerdata_init("stub") == 1);
    playerdata_set_world_id(1);

    test_the_milestone_pass_takes_only_milestones();
    test_the_full_sweep_takes_both();
    test_a_failed_milestone_commit_stays_a_milestone();
    test_a_failed_ordinary_commit_stays_ordinary();
    test_more_milestones_than_one_batch();
    test_logout_settles_both_bits();
    test_the_two_cadences_differ();

    playerdata_close();
    printf("\n%d checks passed\n", passed);
    return 0;
}
