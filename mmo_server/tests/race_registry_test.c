/**
 * @file
 * Exercise the race registry, and with it the modularity contract it exists to keep.
 *
 * The load-bearing test here is the last one: an eleventh race, written only into a
 * fixture, must load and be fully usable with no code change. That is contract M1 as
 * something the build can check, rather than an intention in a design document.
 */

#include "race_registry.h"

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

/** Locate races.json whether the test runs from the repo root or from tests/. */
static const char* races_path(void) {
    static const char* candidates[] = {
        "world_server/data/races.json",
        "../world_server/data/races.json",
        "data/races.json",
    };
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        FILE* f = fopen(candidates[i], "rb");
        if (f) { fclose(f); return candidates[i]; }
    }
    return NULL;
}

/** Verify the shipped registry loads and holds the nine races the bible names. */
static void test_shipped_registry_loads(const char* path) {
    printf("TEST 1: the shipped registry loads\n");

    int count = race_registry_init(path);
    check(count == 9, "all nine races load");

    const RaceDef* wolf = race_get_by_key("wolf");
    check(wolf != NULL, "wolf resolves by key");
    check(wolf && wolf->id == 1, "wolf is race 1");
    check(wolf && strcmp(wolf->latin, "Lupine") == 0, "wolf carries its Latin name");
    check(race_get(1) == wolf, "lookup by id and by key agree");
    check(race_get(0) == NULL, "id 0 names no race");
    check(race_get(MAX_RACES + 1) == NULL, "an id past the cap names no race");
    check(race_get_by_key("gladiator") == NULL, "the retired class names are gone");
}

/** Verify identifiers are unique and dense, which the client's race list relies on. */
static void test_ids_are_unique_and_in_range(void) {
    printf("TEST 2: identifiers are unique and in range\n");

    int seen[MAX_RACES + 1];
    memset(seen, 0, sizeof(seen));

    int count = race_registry_count();
    int duplicates = 0;
    for (int i = 0; i < count; i++) {
        const RaceDef* race = race_at(i);
        if (race->id <= MAX_RACES && seen[race->id]++) duplicates++;
    }
    check(duplicates == 0, "no two races share an identifier");

    /* Ids may have gaps: a removed race keeps its id retired (Snake held 7) so
     * the ids derived from the others, such as quest and dialogue ids, stay put. */
    int out_of_range = 0;
    for (int i = 0; i < count; i++) {
        if (race_at(i)->id < 1 || race_at(i)->id > MAX_RACES) out_of_range++;
    }
    check(out_of_range == 0, "every identifier lies in 1..MAX_RACES");
    check(race_get(7) == NULL, "the retired Snake id 7 resolves to nothing");
    check(race_at(count) == NULL, "enumerating past the end yields nothing");
}

/** Verify exactly the three designed races are offered at character creation. */
static void test_playability(void) {
    printf("TEST 3: only the designed races are playable\n");

    int playable = 0;
    for (int i = 0; i < race_registry_count(); i++) {
        if (race_at(i)->playable) playable++;
    }
    check(playable == 3, "exactly three races are playable");

    check(race_is_playable(race_get_by_key("wolf")->id), "wolf is playable");
    check(race_is_playable(race_get_by_key("bear")->id), "bear is playable");
    check(race_is_playable(race_get_by_key("deer")->id), "deer is playable");
    check(!race_is_playable(race_get_by_key("fox")->id), "fox is not playable");
    check(!race_is_playable(0), "an unknown id is not playable");
    check(!race_is_playable(9999), "an out-of-range id is not playable");
}

/** Verify every spec names a valid role and every playable race fills its hotbar. */
static void test_specs(void) {
    printf("TEST 4: specs name a role and fill the hotbar\n");

    int bad_role = 0, missing_default = 0, wrong_kit_size = 0, spec_b_with_kit = 0;

    for (int i = 0; i < race_registry_count(); i++) {
        const RaceDef* race = race_at(i);

        if (race->spec_count < 2) missing_default++;

        for (int s = 0; s < race->spec_count; s++) {
            const RaceSpec* spec = race_spec_at(race, s);
            if (spec->role != ROLE_TANK && spec->role != ROLE_DPS && spec->role != ROLE_HEALER) {
                bad_role++;
            }
            /* Spec B is out of scope everywhere this milestone, Wolf Tank included. */
            if (s > 0 && spec->ability_count != 0) spec_b_with_kit++;
        }

        const RaceSpec* def = race_default_spec(race);
        if (!def) { missing_default++; continue; }
        if (race->playable && def->ability_count != MAX_ABILITY_SLOTS) wrong_kit_size++;
    }

    check(bad_role == 0, "every spec names one of tank, dps or healer");
    check(missing_default == 0, "every race has a default spec");
    check(wrong_kit_size == 0, "every playable race's default spec fills all five slots");
    check(spec_b_with_kit == 0, "no spec B carries a kit yet");

    const RaceDef* wolf = race_get_by_key("wolf");
    check(race_default_spec(wolf)->role == ROLE_DPS,
          "wolf's default spec is DPS, not tank");
    check(race_spec_at(wolf, 1)->role == ROLE_TANK, "wolf's spec B is the tank");
    check(race_default_spec(race_get_by_key("bear"))->role == ROLE_TANK, "bear is a tank");
    check(race_default_spec(race_get_by_key("deer"))->role == ROLE_HEALER, "deer is a healer");
    check(race_spec_at(wolf, 99) == NULL, "indexing past a race's specs yields nothing");
}

/** Verify every race's stat blocks are total over all eleven stats. */
static void test_stat_blocks_are_total(void) {
    printf("TEST 5: every race defines all eleven stats\n");

    check(STAT_COUNT == 11, "the stat enum holds eleven entries");

    int missing_key = 0;
    for (int s = 0; s < STAT_COUNT; s++) {
        const char* key = stat_key((StatId)s);
        if (!key || stat_from_key(key) != s) missing_key++;
    }
    check(missing_key == 0, "every StatId round-trips through its JSON key");
    check(stat_key((StatId)STAT_COUNT) == NULL, "an out-of-range stat has no key");
    check(stat_from_key("agility") == -1, "the retired stat names resolve to nothing");
    check(stat_from_key(NULL) == -1, "a NULL key resolves to nothing");

    int empty_base = 0, empty_growth = 0;
    for (int i = 0; i < race_registry_count(); i++) {
        const RaceDef* race = race_at(i);
        for (int s = 0; s < STAT_COUNT; s++) {
            if (race->base_stats[s] <= 0)      empty_base++;
            if (race->per_level_stats[s] <= 0) empty_growth++;
        }
    }
    check(empty_base == 0, "no race leaves a base stat at zero");
    check(empty_growth == 0, "no race leaves a growth rate at zero");
}

/** Verify the level curve folds the per-level tenths in correctly. */
static void test_stat_growth(void) {
    printf("TEST 6: stats grow by tenths of a point per level\n");

    const RaceDef* bear = race_get_by_key("bear");
    int base   = bear->base_stats[STAT_VITALITY];
    int growth = bear->per_level_stats[STAT_VITALITY];

    check(race_stat_at_level(bear, STAT_VITALITY, 1) == base, "level 1 is the base value");
    check(race_stat_at_level(bear, STAT_VITALITY, 0) == base, "level 0 clamps to level 1");
    check(race_stat_at_level(bear, STAT_VITALITY, 11) == base + growth,
          "ten levels of growth adds exactly the per-level rate");
    check(race_stat_at_level(bear, STAT_VITALITY, 30) == base + (growth * 29) / 10,
          "level 30 folds twenty-nine levels of tenths");
    check(race_stat_at_level(NULL, STAT_VITALITY, 10) == 0, "an unknown race has no stats");
    check(race_stat_at_level(bear, (StatId)STAT_COUNT, 10) == 0, "an unknown stat reads zero");

    const RaceDef* wolf = race_get_by_key("wolf");
    check(race_stat_at_level(bear, STAT_VITALITY, 30) > race_stat_at_level(wolf, STAT_VITALITY, 30),
          "the tank out-scales the striker in health");
    check(race_stat_at_level(wolf, STAT_STRENGTH, 30) > race_stat_at_level(
              race_get_by_key("deer"), STAT_STRENGTH, 30),
          "the striker out-scales the healer in physical damage");
}

/** Verify role determines resource, so a race never configures its own pool. */
static void test_role_to_resource_is_total(void) {
    printf("TEST 7: role selects the resource for every race and spec\n");

    check(role_resource(ROLE_TANK)   == RESOURCE_RAGE,    "tanks build rage");
    check(role_resource(ROLE_DPS)    == RESOURCE_STAMINA, "damage dealers spend stamina");
    check(role_resource(ROLE_HEALER) == RESOURCE_MANA,    "healers spend mana");
    check(role_resource((CombatRole)99) == RESOURCE_NONE, "an unknown role spends nothing");

    check(role_resource_stat(ROLE_TANK)   == STAT_ENDURANCE,        "endurance sizes rage");
    check(role_resource_stat(ROLE_DPS)    == STAT_STAMINA_CAPACITY, "capacity sizes stamina");
    check(role_resource_stat(ROLE_HEALER) == STAT_FOCUS,            "focus sizes mana");

    int unmapped = 0;
    for (int i = 0; i < race_registry_count(); i++) {
        const RaceDef* race = race_at(i);
        for (int s = 0; s < race->spec_count; s++) {
            if (role_resource(race_spec_at(race, s)->role) == RESOURCE_NONE) unmapped++;
        }
    }
    check(unmapped == 0, "every race and spec resolves to a real resource");
}

/** Verify the passive is data, and that only the undesigned race lacks one. */
static void test_passives(void) {
    printf("TEST 8: passives are data, not code\n");

    const RaceDef* bear = race_get_by_key("bear");
    check(strcmp(bear->passive.key, "thick_hide") == 0, "bear's passive is thick_hide");
    check(bear->passive.description[0] != '\0', "the passive carries a description");

    int named = 0;
    for (int i = 0; i < race_registry_count(); i++) {
        if (race_at(i)->passive.key[0]) named++;
    }
    check(named == 9, "every race names a passive");
}

/**
 * Verify M1: an eleventh race loads from a fixture with no code change at all.
 *
 * This is the whole modularity contract in one test. The fixture is written here,
 * loaded through the same entry point the server uses, and every accessor is then
 * asked about it — including the ones that would need a new case in a switch if the
 * registry were still a compiled table.
 */
static void test_adding_a_race_needs_no_code(void) {
    printf("TEST 9: an eleventh race loads with no code change (contract M1)\n");

    const char* path = "/tmp/race_registry_test_fixture.json";
    FILE* f = fopen(path, "wb");
    if (!f) { check(0, "the fixture could be written"); return; }

    fputs(
        "{\"races\": ["
        " {\"id\": 1, \"key\": \"wolf\", \"name\": \"Wolf\", \"latin\": \"Lupine\","
        "  \"playable\": true,"
        "  \"passive\": {\"key\": \"pack_sense\", \"name\": \"Pack Sense\","
        "                \"description\": \"Scales with allies.\"},"
        "  \"specs\": [{\"id\": \"a\", \"role\": \"dps\", \"default\": true,"
        "               \"abilities\": [\"wolf_bite\"]}],"
        "  \"base_move_speed\": 225,"
        "  \"stats\": {\"base\": {\"strength\": 12}, \"per_level\": {\"strength\": 30}}},"
        " {\"id\": 11, \"key\": \"otter\", \"name\": \"Otter\", \"latin\": \"Lutrine\","
        "  \"playable\": true,"
        "  \"passive\": {\"key\": \"riverborn\", \"name\": \"Riverborn\","
        "                \"description\": \"Swims without slowing.\"},"
        "  \"specs\": [{\"id\": \"a\", \"role\": \"healer\", \"default\": true,"
        "               \"abilities\": [\"otter_mend\", \"otter_splash\"]},"
        "              {\"id\": \"b\", \"role\": \"dps\", \"unlock_level\": 20,"
        "               \"abilities\": []}],"
        "  \"base_move_speed\": 240,"
        "  \"stats\": {\"base\": {\"focus\": 14, \"vitality\": 7},"
        "              \"per_level\": {\"focus\": 33, \"vitality\": 19}}}"
        "]}", f);
    fclose(f);

    int count = race_registry_init(path);
    check(count == 2, "the fixture's two races load");

    const RaceDef* otter = race_get_by_key("otter");
    check(otter != NULL, "the new race resolves by key");
    check(otter && otter->id == 11, "a non-contiguous id is accepted");
    check(otter && race_get(11) == otter, "and resolves by id");
    check(race_is_playable(11), "the new race is offered at creation");
    check(otter && strcmp(otter->passive.name, "Riverborn") == 0, "its passive came through");
    check(otter && otter->spec_count == 2, "both of its specs loaded");
    check(otter && race_default_spec(otter)->role == ROLE_HEALER, "its default spec is the healer");
    check(role_resource(race_default_spec(otter)->role) == RESOURCE_MANA,
          "the resource follows from the role with no per-race configuration");
    check(otter && race_default_spec(otter)->ability_count == 2, "its kit was read");
    check(otter && strcmp(race_default_spec(otter)->ability_keys[0], "otter_mend") == 0,
          "its ability keys were read in order");
    check(race_stat_at_level(otter, STAT_FOCUS, 1) == 14,
          "its level-1 focus is its base value");
    check(race_stat_at_level(otter, STAT_FOCUS, 11) == 14 + 33,
          "ten levels of 3.3 points adds 33, by the same code as every other race");
    check(race_stat_at_level(otter, STAT_STRENGTH, 30) == 0,
          "a stat it omits reads zero rather than inheriting another race's");

    remove(path);
}

/** Verify malformed and hostile registries are refused rather than half-loaded. */
static void test_rejects_bad_data(void) {
    printf("TEST 10: bad registry data is refused, not half-loaded\n");

    const char* path = "/tmp/race_registry_test_bad.json";

    FILE* f = fopen(path, "wb");
    if (!f) { check(0, "the fixture could be written"); return; }
    fputs(
        "{\"races\": ["
        " {\"id\": 0,  \"key\": \"zero\",  \"specs\": []},"
        " {\"id\": 99, \"key\": \"toobig\", \"specs\": []},"
        " {\"id\": 4,  \"specs\": []},"
        " {\"id\": 5,  \"key\": \"first\","
        "  \"specs\": [{\"id\": \"a\", \"role\": \"dps\", \"default\": true, \"abilities\": []}],"
        "  \"stats\": {\"base\": {\"strength\": 5, \"agility\": 9}, \"per_level\": {}}},"
        " {\"id\": 5,  \"key\": \"duplicate\", \"specs\": []},"
        " {\"id\": 6,  \"key\": \"badrole\","
        "  \"specs\": [{\"id\": \"a\", \"role\": \"bard\", \"abilities\": []}]}"
        "]}", f);
    fclose(f);

    int count = race_registry_init(path);
    check(count == 2, "only the two well-formed races are installed");
    check(race_get(0) == NULL, "id 0 is refused");
    check(race_get_by_key("toobig") == NULL, "an id past MAX_RACES is refused");
    check(race_get_by_key("duplicate") == NULL, "the second race claiming id 5 is refused");
    check(race_get_by_key("first") != NULL, "the first race claiming id 5 is kept");
    check(race_get_by_key("first")->base_stats[STAT_STRENGTH] == 5,
          "its recognised stats loaded");
    check(race_get_by_key("badrole") != NULL, "a race with only a bad spec still loads");
    check(race_get_by_key("badrole")->spec_count == 0, "but its unrecognised spec is dropped");
    check(race_default_spec(race_get_by_key("badrole")) == NULL,
          "and it reports no default spec rather than a wrong one");
    remove(path);

    check(race_registry_init("/nonexistent/races.json") == 0, "a missing file loads nothing");
    check(race_registry_count() == 0, "and leaves the registry empty");
    check(race_get(1) == NULL, "with no stale race behind it");
}

int main(void) {
    printf("=== race registry test ===\n\n");

    const char* path = races_path();
    if (!path) {
        printf("SKIPPED: races.json not found; run from the repository root\n");
        return 0;
    }

    test_shipped_registry_loads(path);
    test_ids_are_unique_and_in_range();
    test_playability();
    test_specs();
    test_stat_blocks_are_total();
    test_stat_growth();
    test_role_to_resource_is_total();
    test_passives();
    test_adding_a_race_needs_no_code();
    test_rejects_bad_data();

    race_registry_cleanup();

    printf("\n");
    if (g_failures) {
        printf("%d ASSERTION(S) FAILED\n", g_failures);
        return 1;
    }
    printf("ALL ASSERTIONS PASSED\n");
    return 0;
}
