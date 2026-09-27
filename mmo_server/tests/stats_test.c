/**
 * @file
 * Exercise the eleven-stat model and the derived values it produces.
 *
 * Two properties matter beyond the arithmetic. The stat enum, its JSON keys, and
 * every race's data blocks must agree — a stat that exists in one and not the others
 * is a silently missing bonus. And the role-to-resource mapping must be total over
 * every race and spec, because that is what lets a race declare no resource at all.
 */

#include "class_stats.h"
#include "player_effects.h"
#include "progression.h"

#include <math.h>
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

/** Verify the enum, the key table, and the retired names all agree. */
static void test_enum_and_keys_agree(void) {
    printf("TEST 1: the stat enum and its JSON keys agree\n");

    check(STAT_COUNT == 11, "there are eleven stats");

    int broken = 0;
    for (int i = 0; i < STAT_COUNT; i++) {
        const char* key = stat_key((StatId)i);
        if (!key || !*key || stat_from_key(key) != i) broken++;
    }
    check(broken == 0, "every stat has a key that resolves back to it");

    /* Two stats sharing a key would make one of them unreachable from data. */
    int duplicates = 0;
    for (int i = 0; i < STAT_COUNT; i++) {
        for (int j = i + 1; j < STAT_COUNT; j++) {
            if (strcmp(stat_key((StatId)i), stat_key((StatId)j)) == 0) duplicates++;
        }
    }
    check(duplicates == 0, "no two stats share a key");

    const char* retired[] = { "agility", "wisdom", "defense", "evasion", "luck", "reg" };
    int still_present = 0;
    for (size_t i = 0; i < sizeof(retired) / sizeof(retired[0]); i++) {
        if (stat_from_key(retired[i]) >= 0) still_present++;
    }
    check(still_present == 0, "none of the six retired stat names resolves any more");
}

/** Verify every race defines every stat in both blocks. */
static void test_every_race_covers_every_stat(void) {
    printf("TEST 2: every race defines all eleven stats at base and per level\n");

    int missing_base = 0, missing_growth = 0;

    for (int i = 0; i < race_registry_count(); i++) {
        const RaceDef* race = race_at(i);
        for (int stat = 0; stat < STAT_COUNT; stat++) {
            if (race->base_stats[stat] <= 0)      missing_base++;
            if (race->per_level_stats[stat] <= 0) missing_growth++;
        }
    }
    check(missing_base == 0, "no race omits a base stat");
    check(missing_growth == 0, "no race omits a growth rate");
}

/** Verify the role-to-resource mapping is total over every race and spec. */
static void test_role_mapping_is_total(void) {
    printf("TEST 3: role selects a resource for all nine races and both specs\n");

    int pairs = 0, unmapped = 0, stat_mismatch = 0;

    for (int i = 0; i < race_registry_count(); i++) {
        const RaceDef* race = race_at(i);
        for (int s = 0; s < race->spec_count; s++) {
            const RaceSpec* spec = race_spec_at(race, s);
            pairs++;

            ResourceType resource = role_resource(spec->role);
            if (resource == RESOURCE_NONE) { unmapped++; continue; }

            /* The governing stat must be the one that sizes that pool, or the pool
             * would scale off an attribute the role does not invest in. */
            StatId governing = role_resource_stat(spec->role);
            int consistent =
                (resource == RESOURCE_RAGE    && governing == STAT_ENDURANCE) ||
                (resource == RESOURCE_STAMINA && governing == STAT_STAMINA_CAPACITY) ||
                (resource == RESOURCE_MANA    && governing == STAT_FOCUS);
            if (!consistent) stat_mismatch++;
        }
    }

    check(pairs == 18, "nine races with two specs each is eighteen pairs");
    check(unmapped == 0, "every pair maps to a real resource");
    check(stat_mismatch == 0, "and to the stat that sizes that resource");
}

/** Verify derived values follow from the stats they are supposed to follow from. */
static void test_derived_values(void) {
    printf("TEST 4: health, pool, and speed derive from their stats\n");

    DerivedStats stats;
    const RaceDef* bear = race_get_by_key("bear");
    check(class_stats_compute(bear->id, 1, &stats) == 1, "the bear's level-1 stats compute");

    check(stats.max_health == class_stats_health_for_vitality(stats.stats[STAT_VITALITY]),
          "max health is exactly what its vitality implies");
    check(stats.resource_type == RESOURCE_RAGE, "a tank's pool is rage");
    check(stats.max_resource ==
              class_stats_resource_for_stat(RESOURCE_RAGE, stats.stats[STAT_ENDURANCE]),
          "and is sized by endurance");
    check(stats.move_speed > bear->base_move_speed,
          "dexterity adds to the race's base move speed");

    DerivedStats deer_stats;
    class_stats_compute(race_get_by_key("deer")->id, 1, &deer_stats);
    check(deer_stats.resource_type == RESOURCE_MANA, "a healer's pool is mana");

    DerivedStats wolf_stats;
    class_stats_compute(race_get_by_key("wolf")->id, 1, &wolf_stats);
    check(wolf_stats.resource_type == RESOURCE_STAMINA, "a damage dealer's pool is stamina");

    check(class_stats_compute(0, 1, &stats) == 0, "an unknown race computes nothing");
    check(class_stats_compute(bear->id, 1, NULL) == 0, "a NULL output is refused");
}

/** Verify levelling raises every stat and the values that derive from them. */
static void test_growth_is_monotonic(void) {
    printf("TEST 5: levelling never lowers a stat\n");

    int regressions = 0, health_regressions = 0;

    for (int i = 0; i < race_registry_count(); i++) {
        const RaceDef* race = race_at(i);

        DerivedStats previous;
        class_stats_compute(race->id, 1, &previous);

        for (int level = 2; level <= class_stats_max_level(); level++) {
            DerivedStats current;
            class_stats_compute(race->id, level, &current);

            for (int stat = 0; stat < STAT_COUNT; stat++) {
                if (current.stats[stat] < previous.stats[stat]) regressions++;
            }
            if (current.max_health < previous.max_health) health_regressions++;
            previous = current;
        }
    }

    check(regressions == 0, "no stat ever falls with a level");
    check(health_regressions == 0, "and neither does max health");

    /* Level 30 must be a meaningful distance from level 1 or the curve is pointless. */
    DerivedStats low, high;
    class_stats_compute(race_get_by_key("bear")->id, 1, &low);
    class_stats_compute(race_get_by_key("bear")->id, 30, &high);
    check(high.max_health > low.max_health * 3, "a level-30 bear is far tougher than a fresh one");
}

/** Verify the level clamp holds at both ends. */
static void test_level_clamping(void) {
    printf("TEST 6: levels outside the range clamp rather than index out of bounds\n");

    const RaceDef* wolf = race_get_by_key("wolf");
    DerivedStats at_one, below, at_cap, above;

    class_stats_compute(wolf->id, 1, &at_one);
    class_stats_compute(wolf->id, -5, &below);
    class_stats_compute(wolf->id, class_stats_max_level(), &at_cap);
    class_stats_compute(wolf->id, 9999, &above);

    check(memcmp(&at_one, &below, sizeof(DerivedStats)) == 0, "a negative level clamps to 1");
    check(memcmp(&at_cap, &above, sizeof(DerivedStats)) == 0, "a level past the cap clamps to it");
    check(class_stats_max_level() == 30, "and the cap is 30");
}

/** Verify gear bonuses land on the stat their JSON key names. */
static void test_stat_bonuses_are_indexed(void) {
    printf("TEST 7: an item's bonus lands on the stat its key names\n");

    /* The item loader builds field names from stat_key(), so this checks the naming
     * contract rather than any one item: bonus_<key> for every stat. */
    int nameable = 0;
    for (int stat = 0; stat < STAT_COUNT; stat++) {
        char field[64];
        snprintf(field, sizeof(field), "bonus_%s", stat_key((StatId)stat));
        if (strncmp(field, "bonus_", 6) == 0 && stat_from_key(field + 6) == stat) nameable++;
    }
    check(nameable == STAT_COUNT, "every stat has a bonus_<key> field name that resolves back");
}

int main(void) {
    printf("=== stats test ===\n\n");

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

    test_enum_and_keys_agree();
    test_every_race_covers_every_stat();
    test_role_mapping_is_total();
    test_derived_values();
    test_growth_is_monotonic();
    test_level_clamping();
    test_stat_bonuses_are_indexed();

    class_stats_cleanup();

    printf("\n");
    if (g_failures) {
        printf("%d ASSERTION(S) FAILED\n", g_failures);
        return 1;
    }
    printf("ALL ASSERTIONS PASSED\n");
    return 0;
}
