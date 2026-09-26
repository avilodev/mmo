/**
 * @file
 * Exercise the percentage-stacking rule that all mitigation in the game runs through.
 *
 * The rule is that modifiers of the same kind sum in percentage space and apply once.
 * Two properties follow, and both are checked here rather than assumed: stacking is
 * exactly additive, and it is order-independent across every permutation of the
 * sources applied.
 */

#include "damage_model.h"
#include "progression.h"

#include <stdio.h>
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

/** Verify Bear's two mitigation sources land on exactly the designed numbers. */
static void test_bear_worked_example(void) {
    printf("TEST 1: bear's mitigation matches the design's worked example\n");

    DamageModifiers bear;

    damage_mods_reset(&bear);
    check(damage_resolve(50, NULL, &bear) == 50, "an unmitigated 50-damage hit lands for 50");

    /* Thick Hide is the always-on race passive. */
    damage_mods_reset(&bear);
    damage_mods_add_taken(&bear, 0.10);
    check(damage_resolve(50, NULL, &bear) == 45, "thick hide alone: a 50 hit lands for 45");

    /* Tough Hide is the timed ability, stacking on top of the passive. */
    damage_mods_add_taken(&bear, 0.10);
    check(damage_resolve(50, NULL, &bear) == 40, "thick hide plus tough hide: 50 lands for 40");
}

/** Verify stacking is additive rather than multiplicative. */
static void test_stacking_is_additive(void) {
    printf("TEST 2: same-kind modifiers sum, they do not compound\n");

    DamageModifiers d;
    damage_mods_reset(&d);
    for (int i = 0; i < 3; i++) damage_mods_add_taken(&d, 0.10);

    check(damage_resolve(100, NULL, &d) == 70, "three sources of -10% give -30%");
    check(damage_resolve(100, NULL, &d) != 72,
          "and not the -27.1% three multiplied reductions would give");

    DamageModifiers a;
    damage_mods_reset(&a);
    damage_mods_add_dealt(&a, 0.25);
    damage_mods_add_dealt(&a, 0.25);
    check(damage_resolve(100, &a, NULL) == 150, "outgoing bonuses sum the same way");
}

/**
 * Verify the result does not depend on the order sources were applied.
 *
 * Every permutation of a mixed set of modifiers must resolve identically. This is
 * the property that makes the rule safe to extend: a new item or passive can add to
 * an accumulator from anywhere without needing to know what else is already there.
 */
static void test_order_independence(void) {
    printf("TEST 3: stacking is order-independent across every permutation\n");

    const double taken[] = { 0.10, 0.15, 0.05, 0.20 };
    const int n = 4;

    int order[] = { 0, 1, 2, 3 };
    int expected = -1;
    int permutations = 0, mismatches = 0;

    /* Enumerate all 24 orderings by repeated rotation of each suffix. */
    for (int a = 0; a < n; a++) {
        for (int b = 0; b < n; b++) {
            for (int c = 0; c < n; c++) {
                for (int e = 0; e < n; e++) {
                    if (a == b || a == c || a == e || b == c || b == e || c == e) continue;
                    order[0] = a; order[1] = b; order[2] = c; order[3] = e;

                    DamageModifiers d;
                    damage_mods_reset(&d);
                    for (int i = 0; i < n; i++) damage_mods_add_taken(&d, taken[order[i]]);
                    damage_mods_add_armor(&d, 3);

                    int result = damage_resolve(200, NULL, &d);
                    permutations++;
                    if (expected < 0) expected = result;
                    else if (result != expected) mismatches++;
                }
            }
        }
    }

    check(permutations == 24, "all 24 orderings were exercised");
    check(mismatches == 0, "every ordering resolves to the same damage");
    check(expected == 97, "and to the value the formula predicts: 200 x 0.50 - 3");
}

/** Verify the summed reduction cannot reach immunity. */
static void test_reduction_clamps(void) {
    printf("TEST 4: stacked reduction clamps short of immunity\n");

    progression_init(NULL);
    double cap = progression_config()->damage_taken_reduction_cap;
    check(cap == 0.80, "the shipped clamp is 80%");

    DamageModifiers d;
    damage_mods_reset(&d);
    for (int i = 0; i < 12; i++) damage_mods_add_taken(&d, 0.10);

    check(d.damage_taken_pct > 1.0, "twelve sources of -10% sum past 100%");
    check(damage_mods_effective_reduction(&d) == cap, "but the applied reduction is the cap");
    check(damage_resolve(100, NULL, &d) == 20, "so a 100-damage hit still lands for 20");

    damage_mods_reset(&d);
    damage_mods_add_taken(&d, 0.80);
    check(damage_resolve(100, NULL, &d) == 20, "reaching the cap exactly is not clamped further");

    damage_mods_reset(&d);
    damage_mods_add_taken(&d, 0.79);
    check(damage_resolve(100, NULL, &d) == 21, "and just under it is untouched");
}

/** Verify a stacked vulnerability increases damage and is not caught by the clamp. */
static void test_vulnerability(void) {
    printf("TEST 5: negative reduction is a vulnerability, not a clamped reduction\n");

    DamageModifiers d;
    damage_mods_reset(&d);
    damage_mods_add_taken(&d, -0.25);
    check(damage_resolve(100, NULL, &d) == 125, "a -25% reduction means 25% more damage taken");

    damage_mods_reset(&d);
    damage_mods_add_taken(&d, 0.30);
    damage_mods_add_taken(&d, -0.10);
    check(damage_resolve(100, NULL, &d) == 80, "a vulnerability cancels part of a reduction");
}

/** Verify armor is flat, applies after the percentages, and never grants immunity. */
static void test_armor_is_flat(void) {
    printf("TEST 6: armor subtracts flatly, after the percentages\n");

    DamageModifiers d;
    damage_mods_reset(&d);
    damage_mods_add_armor(&d, 8);
    check(damage_resolve(50, NULL, &d) == 42, "8 armor takes 8 off a 50-damage hit");

    damage_mods_reset(&d);
    damage_mods_add_taken(&d, 0.20);
    damage_mods_add_armor(&d, 8);
    check(damage_resolve(50, NULL, &d) == 32, "the percentage applies first, then the armor");

    damage_mods_reset(&d);
    damage_mods_add_armor(&d, 500);
    check(damage_resolve(50, NULL, &d) == 1,
          "overwhelming armor still leaves one damage — armor mitigates, it never immunises");

    damage_mods_reset(&d);
    damage_mods_add_armor(&d, 3);
    damage_mods_add_armor(&d, 4);
    check(damage_resolve(50, NULL, &d) == 43, "armor from several sources sums");
}

/** Verify the attacker's bonus and the defender's reduction combine as specified. */
static void test_both_sides(void) {
    printf("TEST 7: the attacker's bonus and the defender's reduction combine\n");

    DamageModifiers attacker, defender;
    damage_mods_reset(&attacker);
    damage_mods_reset(&defender);

    damage_mods_add_dealt(&attacker, 0.50);
    damage_mods_add_taken(&defender, 0.20);
    damage_mods_add_armor(&defender, 10);

    check(damage_resolve(100, &attacker, &defender) == 110, "100 x 1.5 x 0.8 - 10 = 110");
    check(damage_resolve(100, &attacker, NULL) == 150, "a defender with no modifiers takes it all");
    check(damage_resolve(100, NULL, &defender) == 70, "an attacker with no bonus is unscaled");
    check(damage_resolve(100, NULL, NULL) == 100, "neither side modified is the base figure");
}

/** Verify the edge cases that would otherwise become negative or healing damage. */
static void test_edges(void) {
    printf("TEST 8: degenerate inputs resolve to something sane\n");

    DamageModifiers attacker;
    damage_mods_reset(&attacker);

    check(damage_resolve(0, NULL, NULL) == 0, "zero base damage stays zero");
    check(damage_resolve(-5, NULL, NULL) == 0, "negative base damage does not heal");

    damage_mods_add_dealt(&attacker, -2.0);
    check(damage_resolve(100, &attacker, NULL) == 1,
          "a -200% outgoing penalty floors at one, it does not heal the target");

    damage_mods_reset(&attacker);
    damage_mods_add_dealt(&attacker, -1.0);
    check(damage_resolve(100, &attacker, NULL) == 1, "a -100% penalty floors the same way");
}

/** Verify healing ignores mitigation, and that percent-of-max healing behaves. */
static void test_healing(void) {
    printf("TEST 9: healing scales with the healer only\n");

    DamageModifiers healer, target;
    damage_mods_reset(&healer);
    damage_mods_reset(&target);
    damage_mods_add_taken(&target, 0.50);
    damage_mods_add_armor(&target, 20);

    check(healing_resolve(40, NULL) == 40, "an unmodified heal is its base value");

    damage_mods_add_dealt(&healer, 0.25);
    check(healing_resolve(40, &healer) == 50, "a +25% healer heals for 50");
    check(healing_resolve(0, &healer) == 0, "healing nothing heals nothing");
    check(healing_resolve(-10, &healer) == 0, "negative healing does not damage");

    printf("TEST 10: percent-of-max healing\n");
    check(percent_of_max(500, 0.05) == 25, "5% of a 500 pool is 25");
    check(percent_of_max(500, 0.10) == 50, "10% of a 500 pool is 50");
    check(percent_of_max(5, 0.05) == 1, "a tiny pool still heals at least one point");
    check(percent_of_max(0, 0.05) == 0, "an empty pool heals nothing");
    check(percent_of_max(500, 0.0) == 0, "a zero percentage heals nothing");
    check(percent_of_max(500, -0.5) == 0, "a negative percentage heals nothing");
}

/** Verify merging accumulators sums them, which is how gear and buffs combine. */
static void test_merge(void) {
    printf("TEST 11: merging accumulators sums them in percentage space\n");

    DamageModifiers gear, buffs;
    damage_mods_reset(&gear);
    damage_mods_reset(&buffs);

    damage_mods_add_taken(&gear, 0.10);
    damage_mods_add_armor(&gear, 12);
    damage_mods_add_taken(&buffs, 0.15);
    damage_mods_add_dealt(&buffs, 0.20);

    damage_mods_merge(&gear, &buffs);
    check(gear.damage_taken_pct == 0.25, "the reductions summed");
    check(gear.damage_dealt_pct == 0.20, "the outgoing bonus carried across");
    check(gear.armor_flat == 12, "the armor was preserved");

    damage_mods_merge(&gear, NULL);
    check(gear.damage_taken_pct == 0.25, "merging nothing changes nothing");
    damage_mods_merge(NULL, &buffs);
    damage_mods_reset(NULL);
    check(1, "NULL arguments are tolerated throughout");
}

int main(void) {
    printf("=== damage stacking test ===\n\n");

    progression_init(NULL);

    test_bear_worked_example();
    test_stacking_is_additive();
    test_order_independence();
    test_reduction_clamps();
    test_vulnerability();
    test_armor_is_flat();
    test_both_sides();
    test_edges();
    test_healing();
    test_merge();

    printf("\n");
    if (g_failures) {
        printf("%d ASSERTION(S) FAILED\n", g_failures);
        return 1;
    }
    printf("ALL ASSERTIONS PASSED\n");
    return 0;
}
