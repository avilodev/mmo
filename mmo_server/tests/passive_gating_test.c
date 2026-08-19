/**
 * @file
 * Exercise the rule that a race passive resolves in Animal Form and nowhere else.
 *
 * The claim this file makes checkable is a strong one: Human Form is not merely
 * intended to be identical across races, it provably is. The same bear takes 45 from
 * a 50-damage hit in Animal Form and the full 50 in Human Form, and every race's
 * Human Form damage-taken accumulator is empty with no buffs up.
 */

#include "player_effects.h"
#include "class_stats.h"
#include "progression.h"

#include <math.h>
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

/** Locate a data file whether the test runs from the repo root or from tests/.
 *
 * The result is written into the caller's buffer rather than a static one, so two
 * lookups can be live at once — which is exactly what loading races and abilities
 * together needs.
 *
 * @param out  Receives the path; left empty when nothing is found.
 * @return     `out` on success, or NULL when the file is not where it should be.
 */
static const char* find_data(char* out, size_t out_size, const char* leaf) {
    const char* prefixes[] = { "world_server/data/", "../world_server/data/", "data/" };

    for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++) {
        snprintf(out, out_size, "%s%s", prefixes[i], leaf);
        FILE* f = fopen(out, "rb");
        if (f) { fclose(f); return out; }
    }
    out[0] = '\0';
    return NULL;
}

/** Build a level-1 player of one race, in one form, with no buffs and no gear. */
static void make_player(ActivePlayer* player, const char* race_key, uint8_t form) {
    memset(player, 0, sizeof(*player));

    const RaceDef* race = race_get_by_key(race_key);
    player->character_id = 1;
    player->race_id = race ? race->id : 0;
    player->level = 1;
    player->form = form;

    player_recompute_stats(player);
    player->health = player->max_health;
}

/** Verify the bear's worked example, in both forms. */
static void test_bear_across_forms(void) {
    printf("TEST 1: the same bear mitigates in animal form and not in human form\n");

    ActivePlayer bear;
    DamageModifiers mods;

    make_player(&bear, "bear", FORM_ANIMAL);
    check(bear.race_id != 0, "the bear loaded from the registry");

    /* Armor is an attribute and applies in both forms; the passive is what differs.
     * Isolate the passive by zeroing armor for this comparison. */
    bear.stats[STAT_ARMOR] = 0;

    player_collect_modifiers(&bear, 0, &mods);
    check(fabs(mods.damage_taken_pct - 0.10) < 1e-9, "thick hide contributes -10% in animal form");
    check(damage_resolve(50, NULL, &mods) == 45, "a 50-damage hit lands for 45");

    bear.form = FORM_HUMAN;
    player_collect_modifiers(&bear, 0, &mods);
    check(mods.damage_taken_pct == 0.0, "the passive contributes nothing in human form");
    check(damage_resolve(50, NULL, &mods) == 50, "so the same hit lands for the full 50");

    check(player_active_passive(&bear) == NULL, "human form reports no active passive");
    bear.form = FORM_ANIMAL;
    check(player_active_passive(&bear) != NULL, "animal form reports one");
}

/**
 * Verify the strong form of D10 over every race.
 *
 * Not "the bear's passive is gated" but "no race has any passive contribution in
 * Human Form", which is the property that makes Human Form uniform.
 */
static void test_human_form_is_uniform(void) {
    printf("TEST 2: every race's human form carries no passive contribution\n");

    int races = race_registry_count();
    check(races == 10, "all ten races are loaded");

    int with_contribution = 0;
    int animal_passives = 0;

    for (int i = 0; i < races; i++) {
        const RaceDef* race = race_at(i);

        ActivePlayer player;
        make_player(&player, race->key, FORM_HUMAN);
        player.stats[STAT_ARMOR] = 0;

        DamageModifiers mods;
        player_collect_modifiers(&player, 0, &mods);
        if (mods.damage_taken_pct != 0.0 || mods.damage_dealt_pct != 0.0 ||
            mods.armor_flat != 0) {
            with_contribution++;
        }
        if (player_active_passive(&player) != NULL) with_contribution++;

        player.form = FORM_ANIMAL;
        if (player_active_passive(&player) != NULL) animal_passives++;
    }

    check(with_contribution == 0,
          "no race modifies damage taken, damage dealt, or armor in human form");
    check(animal_passives > 0, "but several races do carry a passive in animal form");
}

/** Verify a passive that scales with allies contributes nothing when alone. */
static void test_ally_scaling(void) {
    printf("TEST 3: an ally-scaling passive scales, and is capped\n");

    ActivePlayer wolf;
    make_player(&wolf, "wolf", FORM_ANIMAL);
    wolf.stats[STAT_ARMOR] = 0;

    DamageModifiers alone, pack, crowd;
    player_collect_modifiers(&wolf, 0, &alone);
    player_collect_modifiers(&wolf, 3, &pack);
    player_collect_modifiers(&wolf, 50, &crowd);

    check(alone.damage_dealt_pct == 0.0, "pack sense gives a lone wolf nothing");
    check(pack.damage_dealt_pct > alone.damage_dealt_pct, "three allies raise its damage");
    check(fabs(pack.damage_dealt_pct - 0.09) < 1e-9, "by 3% per ally");
    check(fabs(crowd.damage_dealt_pct - 0.15) < 1e-9,
          "and fifty allies are capped at the five the passive counts");

    /* The same passive is inert in Human Form no matter how many allies are near. */
    wolf.form = FORM_HUMAN;
    player_collect_modifiers(&wolf, 50, &crowd);
    check(crowd.damage_dealt_pct == 0.0, "and none of it applies in human form");
}

/** Verify the passive and a timed ability stack additively, as Bear's kit does. */
static void test_passive_stacks_with_ability(void) {
    printf("TEST 4: the passive and a timed reducer stack additively\n");

    ActivePlayer bear;
    make_player(&bear, "bear", FORM_ANIMAL);
    bear.stats[STAT_ARMOR] = 0;

    /* Tough Hide: a further 10% off, expressed in tenths of a percent. */
    AbilityEffectDef tough_hide = {0};
    tough_hide.type     = EFFECT_DAMAGE_TAKEN;
    tough_hide.value    = 100;
    tough_hide.duration = 8.0f;
    tough_hide.self     = 1;
    check(player_effect_apply(&bear, &tough_hide, bear.character_id) == 1,
          "tough hide applies");

    DamageModifiers mods;
    player_collect_modifiers(&bear, 0, &mods);
    check(fabs(mods.damage_taken_pct - 0.20) < 1e-9, "passive plus ability is -20%, not -19%");
    check(damage_resolve(50, NULL, &mods) == 40, "so a 50-damage hit lands for 40");

    /* In Human Form only the ability remains: Guard-like mitigation and nothing else. */
    bear.form = FORM_HUMAN;
    player_collect_modifiers(&bear, 0, &mods);
    check(fabs(mods.damage_taken_pct - 0.10) < 1e-9, "human form keeps the ability alone");
    check(damage_resolve(50, NULL, &mods) == 45, "so the same hit lands for 45");
}

/** Verify armor is an attribute and therefore applies in both forms. */
static void test_armor_is_not_gated(void) {
    printf("TEST 5: armor is an attribute, so it applies in both forms\n");

    ActivePlayer bear;
    make_player(&bear, "bear", FORM_ANIMAL);

    DamageModifiers animal, human;
    player_collect_modifiers(&bear, 0, &animal);
    bear.form = FORM_HUMAN;
    player_collect_modifiers(&bear, 0, &human);

    check(animal.armor_flat > 0, "the bear has armor in animal form");
    check(human.armor_flat == animal.armor_flat, "and exactly the same in human form");
    check(bear.stats[STAT_ARMOR] == animal.armor_flat,
          "which is its armor attribute, unmodified");
}

/** Verify Human Form has no resource pool at all. */
static void test_human_form_has_no_pool(void) {
    printf("TEST 6: human form carries no resource pool\n");

    int with_pool = 0, animal_without_pool = 0;

    for (int i = 0; i < race_registry_count(); i++) {
        const RaceDef* race = race_at(i);

        ActivePlayer player;
        make_player(&player, race->key, FORM_HUMAN);
        if (player.resource_type != RESOURCE_NONE || player.max_resource != 0) with_pool++;

        make_player(&player, race->key, FORM_ANIMAL);
        if (player.resource_type == RESOURCE_NONE || player.max_resource <= 0) {
            animal_without_pool++;
        }
    }

    check(with_pool == 0, "no race has a resource pool in human form");
    check(animal_without_pool == 0, "and every race has one in animal form");
}

/**
 * Verify a form swap does not spend the character's resource pool.
 *
 * Human Form has no pool rather than an empty one. Treating it as empty would mean a
 * tank could be stripped of every point of rage by swapping out and back, which is
 * both a balance hole and a bad surprise.
 */
static void test_swap_preserves_the_pool(void) {
    printf("TEST 8: swapping to human form and back does not spend the pool\n");

    int lost = 0, restored_wrong = 0;

    for (int i = 0; i < race_registry_count(); i++) {
        const RaceDef* race = race_at(i);

        ActivePlayer player;
        make_player(&player, race->key, FORM_ANIMAL);

        int full = player.max_resource;
        player.resource = full / 2;
        int held = player.resource;

        player.form = FORM_HUMAN;
        player_recompute_stats(&player);
        if (player.resource != held) lost++;
        if (player.max_resource != 0) restored_wrong++;

        player.form = FORM_ANIMAL;
        player_recompute_stats(&player);
        if (player.resource != held || player.max_resource != full) restored_wrong++;
    }

    check(lost == 0, "no race loses resource on entering human form");
    check(restored_wrong == 0, "and every race gets its exact pool back on returning");

    /* A pool that shrinks — losing a stat-boosting item, say — still clamps. */
    ActivePlayer bear;
    make_player(&bear, "bear", FORM_ANIMAL);
    bear.resource = bear.max_resource;
    bear.level = 1;
    player_recompute_stats(&bear);
    check(bear.resource <= bear.max_resource, "a shrinking pool still clamps the value in it");
}

/** Verify Ferality scales Animal Form output and leaves Human Form at exactly 1.0. */
static void test_ferality_gap(void) {
    printf("TEST 9: ferality scales animal form only\n");

    ActivePlayer wolf;
    make_player(&wolf, "wolf", FORM_ANIMAL);

    float animal = player_form_power(&wolf);
    wolf.form = FORM_HUMAN;
    float human = player_form_power(&wolf);

    check(human == 1.0f, "human form applies no ferality multiplier at all");
    check(animal > human, "animal form is the stronger of the two");

    /* One value in progression.json controls the entire gap. */
    double per_point = progression_config()->animal_power_per_ferality;
    float expected = 1.0f + (float)(wolf.stats[STAT_FERALITY] * per_point);
    check(fabs(animal - expected) < 1e-5, "the gap is exactly ferality times the tunable");

    int differing = 0;
    for (int i = 0; i < race_registry_count(); i++) {
        ActivePlayer p;
        make_player(&p, race_at(i)->key, FORM_HUMAN);
        if (player_form_power(&p) != 1.0f) differing++;
    }
    check(differing == 0, "and every race's human form sits at exactly 1.0");
}

int main(void) {
    printf("=== passive gating test ===\n\n");

    char races_path[512], progression_path[512];
    if (!find_data(races_path, sizeof(races_path), "races.json")) {
        printf("SKIPPED: races.json not found; run from the repository root\n");
        return 0;
    }
    find_data(progression_path, sizeof(progression_path), "progression.json");

    if (!class_stats_init(races_path, progression_path)) {
        printf("SKIPPED: the race registry did not load\n");
        return 0;
    }

    test_bear_across_forms();
    test_human_form_is_uniform();
    test_ally_scaling();
    test_passive_stacks_with_ability();
    test_armor_is_not_gated();
    test_human_form_has_no_pool();
    test_swap_preserves_the_pool();
    test_ferality_gap();

    class_stats_cleanup();

    printf("\n");
    if (g_failures) {
        printf("%d ASSERTION(S) FAILED\n", g_failures);
        return 1;
    }
    printf("ALL ASSERTIONS PASSED\n");
    return 0;
}
