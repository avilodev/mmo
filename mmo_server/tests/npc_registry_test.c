/**
 * @file
 * Prove the registry's rules fire, using fixtures written at run time.
 *
 * npc_content_test.c proves the shipped content is clean. That is only worth
 * anything if dirty content is actually rejected, so every case here is content
 * that *should* fail, plus the two properties the design turns on:
 *
 *   - there is no compiled ceiling on how many types load (the format this
 *     replaced dropped everything past 32 with a log line);
 *   - per-type sizing is derived from the content rather than declared.
 */

#include "npc_registry.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures;
static char g_dir[256];

static void check(int condition, const char* what) {
    if (condition) {
        printf("  ok   %s\n", what);
    } else {
        printf("  FAIL %s\n", what);
        g_failures++;
    }
}

/** Write one fixture file into the scratch directory. */
static void put(const char* name, const char* body) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", g_dir, name);
    FILE* f = fopen(path, "w");
    if (!f) { printf("  FAIL could not write %s\n", path); g_failures++; return; }
    fputs(body, f);
    fclose(f);
}

/** Lay down a minimal valid content set; individual tests then overwrite one file. */
static void put_baseline(void) {
    put("factions.json",
        "{\"factions\":[{\"key\":\"f\",\"id\":1,\"name\":\"F\",\"color\":[0.5,0.5,0.5]}]}");
    put("archetypes.json",
        "{\"archetypes\":[{\"key\":\"melee\",\"movement\":\"follow\",\"move_speed\":200,"
        "\"aggro_tiles\":20,\"leash_tiles\":40,\"preferred_tiles\":1.5},"
        "{\"key\":\"kiter\",\"movement\":\"maintain_range\",\"move_speed\":170,"
        "\"aggro_tiles\":30,\"leash_tiles\":50,\"preferred_tiles\":6}]}");
    put("abilities.json",
        "{\"abilities\":[{\"key\":\"bite\",\"name\":\"Bite\",\"delivery\":\"telegraph\","
        "\"cast_time\":0.3,\"cooldown\":1.0,\"range_tiles\":2,\"damage\":10,"
        "\"telegraph\":{\"shape\":\"rectangle\",\"width_tiles\":1,\"length_tiles\":2}}]}");
    put("affixes.json",
        "{\"affixes\":[{\"key\":\"duelist\",\"name\":\"Duelist\","
        "\"applies_to\":{\"roles\":[\"melee\"]},\"modifiers\":{\"damage_pct\":20}}]}");
    put("types.json",
        "{\"npc_types\":[{\"key\":\"t1\",\"id\":100,\"name\":\"T1\",\"faction\":\"f\","
        "\"archetype\":\"melee\",\"role\":\"melee\",\"health\":20,\"xp_reward\":10,"
        "\"abilities\":[\"bite\"]}]}");
}

/** Replace types.json with one row and report whether validation passed. */
static int validate_with_type(const char* type_json) {
    char buf[2048];
    snprintf(buf, sizeof(buf), "{\"npc_types\":[%s]}", type_json);
    put_baseline();
    put("types.json", buf);
    if (!npc_registry_load(g_dir)) return -1;      /* rejected at load */
    int problems = npc_content_validate();
    npc_registry_cleanup();
    return problems;
}

int main(int argc, char** argv) {
    log_init();
    snprintf(g_dir, sizeof(g_dir), "%s", (argc > 1) ? argv[1] : "/tmp/npc_fixtures");

    char cmd[512];
    snprintf(cmd, sizeof(cmd), "mkdir -p '%s'", g_dir);
    if (system(cmd) != 0) { printf("could not create %s\n", g_dir); return 1; }

    printf("TEST 1: a clean fixture set loads and validates\n");
    put_baseline();
    check(npc_registry_load(g_dir) == 1, "baseline content loads");
    check(npc_content_validate() == 0, "baseline content validates");
    npc_registry_cleanup();

    printf("TEST 2: unresolved references are refused at load, not skipped\n");
    put_baseline();
    put("types.json",
        "{\"npc_types\":[{\"key\":\"t1\",\"id\":100,\"name\":\"T1\",\"faction\":\"f\","
        "\"archetype\":\"melee\",\"role\":\"melee\",\"health\":20,\"xp_reward\":10,"
        "\"abilities\":[\"no_such_ability\"]}]}");
    check(npc_registry_load(g_dir) == 0, "an unknown ability key fails the load");
    npc_registry_cleanup();

    put_baseline();
    put("types.json",
        "{\"npc_types\":[{\"key\":\"t1\",\"id\":100,\"name\":\"T1\",\"faction\":\"f\","
        "\"archetype\":\"no_such_archetype\",\"role\":\"melee\",\"health\":20,"
        "\"xp_reward\":10,\"abilities\":[\"bite\"]}]}");
    check(npc_registry_load(g_dir) == 0, "an unknown archetype fails the load");
    npc_registry_cleanup();

    printf("TEST 3: an override may retune a scalar but not change a shape\n");
    put_baseline();
    put("types.json",
        "{\"npc_types\":[{\"key\":\"t1\",\"id\":100,\"name\":\"T1\",\"faction\":\"f\","
        "\"archetype\":\"melee\",\"role\":\"melee\",\"health\":20,\"xp_reward\":10,"
        "\"abilities\":[{\"key\":\"bite\",\"damage\":99}]}]}");
    check(npc_registry_load(g_dir) == 1, "a scalar override loads");
    {
        const NPCTypeDef* t = npc_type_get_by_key("t1");
        const NPCAbilityDefn* a = t ? npc_type_ability(t, 0) : NULL;
        check(a && a->damage == 99, "and applies to the instance");
        check(npc_ability_by_key("bite")->damage == 10, "leaving the shared row alone");
    }
    npc_registry_cleanup();

    put_baseline();
    put("types.json",
        "{\"npc_types\":[{\"key\":\"t1\",\"id\":100,\"name\":\"T1\",\"faction\":\"f\","
        "\"archetype\":\"melee\",\"role\":\"melee\",\"health\":20,\"xp_reward\":10,"
        "\"abilities\":[{\"key\":\"bite\",\"delivery\":\"projectile\"}]}]}");
    check(npc_registry_load(g_dir) == 0, "overriding delivery is refused");
    npc_registry_cleanup();

    printf("TEST 4: R7 catches a fightable enemy that can do nothing\n");
    /* R7 narrowed deliberately. It began as "no ability reaches the distance
     * this archetype holds at", which caught a real bug on the first seven types
     * and then started failing correct content the moment a type carried a mixed
     * kit -- a support caster that holds at five tiles and also has a melee swing
     * is not broken, it has an answer for something that closed on it.
     *
     * The fix was in the engine: a profile's hold distance is now clamped to its
     * kit's longest reach, so an enemy can no longer stand outside its own range.
     * npc_behavior_test proves that clamp. What is still checkable here, and
     * still silent, is a fightable enemy with nothing usable at all. */
    int r7 = validate_with_type(
        "{\"key\":\"t1\",\"id\":100,\"name\":\"T1\",\"faction\":\"f\","
        "\"archetype\":\"kiter\",\"role\":\"ranged\",\"health\":20,\"xp_reward\":10,"
        "\"abilities\":[]}");
    check(r7 > 0, "an enemy with no abilities at all is rejected");

    int mixed = validate_with_type(
        "{\"key\":\"t1\",\"id\":100,\"name\":\"T1\",\"faction\":\"f\","
        "\"archetype\":\"kiter\",\"role\":\"ranged\",\"health\":20,\"xp_reward\":10,"
        "\"abilities\":[\"bite\"]}");
    check(mixed == 0,
          "and a short attack on a far-holding archetype is not -- the engine closes the gap");

    printf("TEST 5: metadata rules fire\n");
    check(validate_with_type(
        "{\"key\":\"t1\",\"id\":100,\"name\":\"T1\",\"faction\":\"f\","
        "\"archetype\":\"melee\",\"role\":\"melee\",\"health\":20,\"xp_reward\":0,"
        "\"abilities\":[\"bite\"]}") > 0, "R12: a fightable enemy awarding no XP is rejected");

    check(validate_with_type(
        "{\"key\":\"t1\",\"id\":5,\"name\":\"T1\",\"faction\":\"f\","
        "\"archetype\":\"melee\",\"role\":\"melee\",\"health\":20,\"xp_reward\":10,"
        "\"abilities\":[\"bite\"]}") > 0, "R4: an id outside the enemy band is rejected");

    check(validate_with_type(
        "{\"key\":\"t1\",\"id\":100,\"name\":\"T1\",\"faction\":\"f\","
        "\"archetype\":\"melee\",\"role\":\"melee\",\"health\":20,\"xp_reward\":10,"
        "\"move_speed\":60,\"abilities\":[\"bite\"]}") > 0,
        "R14: a melee chaser slower than a walking player is rejected");

    check(validate_with_type(
        "{\"key\":\"t1\",\"id\":100,\"name\":\"T1\",\"faction\":\"f\","
        "\"archetype\":\"melee\",\"role\":\"ranged\",\"health\":20,\"xp_reward\":10,"
        "\"abilities\":[\"bite\"],\"affixes\":[\"duelist\"]}") > 0,
        "R11: an affix on a role it does not apply to is rejected");

    check(validate_with_type(
        "{\"key\":\"t1\",\"id\":100,\"name\":\"T1\",\"faction\":\"f\","
        "\"archetype\":\"melee\",\"role\":\"melee\",\"health\":20,\"xp_reward\":10,"
        "\"abilities\":[\"bite\"],"
        "\"triggers\":[{\"when\":\"hp_below\",\"value\":150,\"action\":\"modify\"}]}") > 0,
        "R10: an hp_below threshold outside (0,100) is rejected");

    printf("TEST 6: duplicate ids are caught\n");
    put_baseline();
    put("types.json",
        "{\"npc_types\":["
        "{\"key\":\"a\",\"id\":100,\"name\":\"A\",\"faction\":\"f\",\"archetype\":\"melee\","
        "\"role\":\"melee\",\"health\":20,\"xp_reward\":10,\"abilities\":[\"bite\"]},"
        "{\"key\":\"b\",\"id\":100,\"name\":\"B\",\"faction\":\"f\",\"archetype\":\"melee\","
        "\"role\":\"melee\",\"health\":20,\"xp_reward\":10,\"abilities\":[\"bite\"]}]}");
    check(npc_registry_load(g_dir) == 1, "two rows sharing an id still parse");
    check(npc_content_validate() > 0, "but validation rejects the collision");
    npc_registry_cleanup();

    printf("TEST 7: nothing caps how much content loads\n");
    {
        /* 300 types is ten times the old MAX_NPC_AI_PROFILES of 32, which dropped
         * the surplus with a log line that read in game as a mob that would not
         * aggro. Nothing here is sized at compile time, so this simply loads. */
        size_t cap = 300 * 256 + 64;
        char* buf = malloc(cap);
        check(buf != NULL, "fixture buffer allocates");
        if (buf) {
            size_t n = (size_t)snprintf(buf, cap, "{\"npc_types\":[");
            for (int i = 0; i < 300; i++)
                n += (size_t)snprintf(buf + n, cap - n,
                    "%s{\"key\":\"t%d\",\"id\":%d,\"name\":\"T%d\",\"faction\":\"f\","
                    "\"archetype\":\"melee\",\"role\":\"melee\",\"health\":20,"
                    "\"xp_reward\":10,\"abilities\":[\"bite\"]}",
                    i ? "," : "", i, 100 + i, i);
            snprintf(buf + n, cap - n, "]}");

            put_baseline();
            put("types.json", buf);
            free(buf);

            check(npc_registry_load(g_dir) == 1, "300 types load without a cap");
            check(npc_type_count() == 300, "and all 300 are present, none dropped");
            npc_registry_cleanup();
        }
    }

    printf("TEST 8: per-NPC sizing is derived from content, not declared\n");
    put_baseline();
    put("abilities.json",
        "{\"abilities\":["
        "{\"key\":\"a1\",\"delivery\":\"telegraph\",\"cooldown\":1,\"range_tiles\":2,"
        "\"damage\":5,\"telegraph\":{\"shape\":\"circle\",\"radius_tiles\":1}},"
        "{\"key\":\"a2\",\"delivery\":\"telegraph\",\"cooldown\":1,\"range_tiles\":2,"
        "\"damage\":5,\"telegraph\":{\"shape\":\"circle\",\"radius_tiles\":1}},"
        "{\"key\":\"a3\",\"delivery\":\"telegraph\",\"cooldown\":1,\"range_tiles\":2,"
        "\"damage\":5,\"telegraph\":{\"shape\":\"circle\",\"radius_tiles\":1}},"
        "{\"key\":\"a4\",\"delivery\":\"telegraph\",\"cooldown\":1,\"range_tiles\":2,"
        "\"damage\":5,\"telegraph\":{\"shape\":\"circle\",\"radius_tiles\":1}},"
        "{\"key\":\"a5\",\"delivery\":\"telegraph\",\"cooldown\":1,\"range_tiles\":2,"
        "\"damage\":5,\"telegraph\":{\"shape\":\"circle\",\"radius_tiles\":1}},"
        "{\"key\":\"a6\",\"delivery\":\"telegraph\",\"cooldown\":1,\"range_tiles\":2,"
        "\"damage\":5,\"telegraph\":{\"shape\":\"circle\",\"radius_tiles\":1}},"
        "{\"key\":\"a7\",\"delivery\":\"telegraph\",\"cooldown\":1,\"range_tiles\":2,"
        "\"damage\":5,\"telegraph\":{\"shape\":\"circle\",\"radius_tiles\":1}}]}");
    put("types.json",
        "{\"npc_types\":["
        "{\"key\":\"small\",\"id\":100,\"name\":\"S\",\"faction\":\"f\",\"archetype\":\"melee\","
        "\"role\":\"melee\",\"health\":20,\"xp_reward\":10,\"abilities\":[\"a1\"]},"
        "{\"key\":\"boss\",\"id\":101,\"name\":\"B\",\"faction\":\"f\",\"archetype\":\"melee\","
        "\"role\":\"miniboss\",\"health\":150,\"xp_reward\":100,"
        "\"abilities\":[\"a1\",\"a2\",\"a3\",\"a4\",\"a5\",\"a6\",\"a7\"]}]}");
    check(npc_registry_load(g_dir) == 1, "a 7-ability mini-boss loads");
    check(npc_registry_max_abilities() == 7,
          "max_abilities reports 7 -- past the old MAX_NPC_ABILITIES of 4");
    check(npc_content_validate() == 0, "and the content still validates");
    npc_registry_cleanup();

    printf("\n%s: %d failure(s)\n", g_failures ? "FAILED" : "PASSED", g_failures);
    return g_failures ? 1 : 0;
}
