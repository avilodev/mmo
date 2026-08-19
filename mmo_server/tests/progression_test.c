/**
 * @file
 * Exercise the level and experience curve, and the contract that it stays tunable.
 *
 * The curve is geometric and every number in it comes from progression.json. The
 * last test is the point of the file: changing the growth factor in a fixture must
 * change the whole table, with no recompile. That is contract M3 made checkable.
 */

#include "progression.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures = 0;

/** Record one assertion result and print it. */
static void check(int condition, const char* what) {
    if (condition) {
        printf("  ok   %s\n", what);
    } else {
        printf("  FAIL %s\n", what);
        g_failures++;
    }
}

/** Locate progression.json whether the test runs from the repo root or from tests/. */
static const char* progression_path(void) {
    static const char* candidates[] = {
        "world_server/data/progression.json",
        "../world_server/data/progression.json",
        "data/progression.json",
    };
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        FILE* f = fopen(candidates[i], "rb");
        if (f) { fclose(f); return candidates[i]; }
    }
    return NULL;
}

/** Write a fixture and load it, returning whether the load reported success. */
static int load_fixture(const char* path, const char* body) {
    FILE* f = fopen(path, "wb");
    if (!f) return -1;
    fputs(body, f);
    fclose(f);
    return progression_init(path);
}

/** Verify the shipped curve hits the numbers the design was tuned against. */
static void test_shipped_curve(const char* path) {
    printf("TEST 1: the shipped curve matches its design targets\n");

    check(progression_init(path) == 1, "progression.json loads");
    check(progression_max_level() == 30, "the level cap is 30, not the old 50");

    const uint64_t want[] = { 100, 150, 225, 338, 506 };
    int wrong = 0;
    for (int level = 2; level <= 6; level++) {
        if (progression_xp_step(level) != want[level - 2]) wrong++;
    }
    check(wrong == 0, "levels 2-6 cost 100/150/225/338/506 at growth 1.5");

    uint64_t cap_step = progression_xp_step(30);
    check(cap_step > 8000000 && cap_step < 9000000, "level 30 costs roughly 8.5M");

    uint64_t cumulative = progression_xp_for_level(30);
    check(cumulative > 25000000 && cumulative < 26000000, "the run to 30 totals roughly 25.6M");
}

/** Verify the table is strictly increasing and its cumulative form is consistent. */
static void test_table_is_coherent(void) {
    printf("TEST 2: the table is monotonic and self-consistent\n");

    int not_increasing = 0, mismatched = 0;
    uint64_t running = 0;

    for (int level = 2; level <= progression_max_level(); level++) {
        uint64_t step  = progression_xp_step(level);
        uint64_t total = progression_xp_for_level(level);

        if (step == 0) not_increasing++;
        if (total <= progression_xp_for_level(level - 1)) not_increasing++;

        running += step;
        if (total != running) mismatched++;
    }

    check(not_increasing == 0, "every level costs more experience than nothing");
    check(mismatched == 0, "the cumulative threshold is the sum of the steps below it");
}

/** Verify the boundaries return sane values instead of reading past the table. */
static void test_boundaries(void) {
    printf("TEST 3: levels outside the table are handled, not indexed\n");

    int cap = progression_max_level();

    check(progression_xp_for_level(0) == 0, "level 0 needs no experience");
    check(progression_xp_for_level(1) == 0, "level 1 needs no experience");
    check(progression_xp_for_level(-5) == 0, "a negative level needs no experience");
    check(progression_xp_for_level(cap + 100) == progression_xp_for_level(cap),
          "past the cap the threshold flattens rather than growing");

    check(progression_xp_step(1) == 0, "level 1 has no step");
    check(progression_xp_step(cap + 1) == 0, "there is no step past the cap");
}

/** Verify the level check advances correctly and never exceeds the cap. */
static void test_check_level(void) {
    printf("TEST 4: the level check advances through every threshold it has passed\n");

    int cap = progression_max_level();

    check(progression_check_level(1, 0) == 1, "no experience stays at level 1");
    check(progression_check_level(1, 99) == 1, "one point short does not level");
    check(progression_check_level(1, 100) == 2, "hitting the threshold exactly levels");
    check(progression_check_level(1, 250) == 3, "250 reaches level 3");
    check(progression_check_level(1, 249) == 2, "249 does not");

    /* A large XP award must be able to cross several thresholds at once, which is
     * what a quest turn-in on a fresh character actually does. */
    check(progression_check_level(1, 1000) == 5, "one award can cross several levels");

    check(progression_check_level(cap, UINT64_MAX) == cap, "the cap holds against any total");
    check(progression_check_level(cap + 5, 0) == cap, "a level past the cap is pulled back");
    check(progression_check_level(0, 0) == 1, "level 0 is treated as level 1");
    check(progression_check_level(1, UINT64_MAX) == cap, "an absurd total stops at the cap");
}

/**
 * Verify M3: retuning the curve is a data change.
 *
 * Nothing here recompiles. A different growth factor, base cost, and cap are written
 * into a fixture and the whole table follows.
 */
static void test_retuning_needs_no_code(void) {
    printf("TEST 5: retuning the curve needs no recompile (contract M3)\n");

    const char* path = "/tmp/progression_test_fixture.json";

    check(load_fixture(path, "{\"max_level\": 12, \"xp\": {\"base\": 200, \"growth\": 2.0}}") == 1,
          "a retuned fixture loads");
    check(progression_max_level() == 12, "the cap moved to 12");
    check(progression_xp_step(2) == 200, "the base cost moved to 200");
    check(progression_xp_step(3) == 400, "growth 2.0 doubles each level");
    check(progression_xp_step(4) == 800, "and keeps doubling");
    check(progression_xp_for_level(4) == 1400, "the cumulative threshold follows");
    check(progression_check_level(1, 1400) == 4, "levelling follows the retuned table");
    check(progression_check_level(1, UINT64_MAX) == 12, "and stops at the retuned cap");

    check(load_fixture(path, "{\"max_level\": 12, \"xp\": {\"base\": 200, \"growth\": 1.0}}") == 1,
          "a flat curve loads");
    check(progression_xp_step(2) == progression_xp_step(11), "growth 1.0 makes every level cost the same");

    remove(path);
}

/** Verify a missing or mistuned file degrades to defaults instead of failing to start. */
static void test_bad_config_falls_back(void) {
    printf("TEST 6: a missing or mistuned file falls back to the shipped defaults\n");

    check(progression_init("/nonexistent/progression.json") == 0, "a missing file reports failure");
    check(progression_max_level() == 30, "but the defaults are in force");
    check(progression_xp_step(2) == 100, "with the shipped curve");
    check(progression_config() != NULL, "the config is never NULL");

    check(progression_init(NULL) == 0, "a NULL path selects the defaults");
    check(progression_max_level() == 30, "which are the shipped values");

    const char* path = "/tmp/progression_test_bad.json";

    /* An uncapped reduction lets enough stacked sources reach immunity. That is a bug
     * class rather than a tuning choice, so the value is refused, not honoured. */
    check(load_fixture(path, "{\"tunables\": {\"damage_taken_reduction_cap\": 1.0}}") == 1,
          "an out-of-range reduction cap still loads the file");
    check(progression_config()->damage_taken_reduction_cap == 0.80,
          "but the cap itself is refused and the default kept");

    check(load_fixture(path, "{\"max_level\": 100000}") == 1, "an absurd cap loads");
    check(progression_max_level() == PROGRESSION_MAX_LEVEL_CAP, "and is clamped to the array bound");

    check(load_fixture(path, "{\"max_level\": 0}") == 1, "a zero cap loads");
    check(progression_max_level() == 1, "and is pulled up to a usable level 1");

    check(load_fixture(path, "{not json") == 0, "malformed JSON reports failure");
    check(progression_max_level() == 30, "and leaves the defaults in force");

    remove(path);
}

/** Verify an unspecified tunable keeps its default rather than reading as zero. */
static void test_partial_config(void) {
    printf("TEST 7: an unlisted tunable keeps its default\n");

    const char* path = "/tmp/progression_test_partial.json";
    check(load_fixture(path, "{\"tunables\": {\"form_swap_cooldown\": 0}}") == 1,
          "a file listing one tunable loads");

    const ProgressionConfig* c = progression_config();
    check(c->form_swap_cooldown == 0.0, "the listed tunable took the new value");
    check(c->health_per_vitality == 12.0, "an unlisted tunable kept its default");
    check(c->damage_taken_reduction_cap == 0.80, "as did the reduction cap");
    check(c->animal_power_per_ferality == 0.01, "as did the Ferality scalar");

    remove(path);
}

/** Verify the shipped tunables are the values the design specifies. */
static void test_shipped_tunables(const char* path) {
    printf("TEST 8: the shipped tunables hold their designed values\n");

    progression_init(path);
    const ProgressionConfig* c = progression_config();

    check(c->damage_taken_reduction_cap == 0.80, "damage reduction clamps at 80%");
    check(c->form_swap_cooldown == 1.5, "the shared form-swap cooldown is 1.5s");
    check(c->health_per_vitality > 0, "vitality contributes to health");
    check(c->mana_per_focus > 0, "focus sizes the mana pool");
    check(c->stamina_per_stamina_capacity > 0, "stamina capacity sizes the stamina pool");
    check(c->rage_per_endurance > 0, "endurance sizes the rage pool");
    check(c->animal_power_per_ferality > 0, "ferality scales animal form output");
    check(c->rage_per_damage_taken > 0, "rage builds from damage taken");
    check(c->rage_decay_per_second > 0, "and decays out of combat");
}

int main(void) {
    printf("=== progression test ===\n\n");

    const char* path = progression_path();
    if (!path) {
        printf("SKIPPED: progression.json not found; run from the repository root\n");
        return 0;
    }

    test_shipped_curve(path);
    test_table_is_coherent();
    test_boundaries();
    test_check_level();
    test_retuning_needs_no_code();
    test_bad_config_falls_back();
    test_partial_config();
    test_shipped_tunables(path);

    printf("\n");
    if (g_failures) {
        printf("%d ASSERTION(S) FAILED\n", g_failures);
        return 1;
    }
    printf("ALL ASSERTIONS PASSED\n");
    return 0;
}
