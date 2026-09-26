/**
 * @file
 * Load the real shipped NPC content files and run every validation rule over them.
 *
 * This is the test that fails when a content author makes a typo, which is why it
 * reads the files the server actually ships rather than fixtures. A missing ability
 * key, an enemy that cannot reach its own attack, a summon loop, an id that collides
 * with a quest NPC -- all of them fail here, in CI, instead of as an enemy standing
 * inert in a live world.
 *
 * The companion npc_registry_test.c uses synthetic fixtures to prove the rules
 * themselves fire; this one proves the shipped content satisfies them.
 */

#include "npc_registry.h"
#include "log.h"

#include <stdio.h>
#include <string.h>

static int g_failures;

static void check(int condition, const char* what) {
    if (condition) {
        printf("  ok   %s\n", what);
    } else {
        printf("  FAIL %s\n", what);
        g_failures++;
    }
}

int main(int argc, char** argv) {
    log_init();

    const char* dir = (argc > 1) ? argv[1] : "world_server/data/npc";
    printf("TEST 1: the shipped content loads\n");

    int loaded = npc_registry_load(dir);
    check(loaded, "npc_registry_load() accepts the shipped files");
    if (!loaded) {
        printf("\n  (looked in '%s'; pass a directory as argv[1] to override)\n", dir);
        return 1;
    }

    check(npc_faction_count()   == 8, "eight factions load");
    check(npc_archetype_count() == 9, "nine archetypes load");
    check(npc_type_count()      >  0, "at least one type loads");
    check(npc_ability_count()   >  0, "at least one ability loads");

    printf("TEST 2: every validation rule passes over the shipped content\n");
    int problems = npc_content_validate();
    check(problems == 0, "npc_content_validate() reports no problems");

    printf("TEST 3: composition resolved rather than merely parsed\n");
    const NPCTypeDef* wolf = npc_type_get_by_key("feral_wolf");
    check(wolf != NULL, "feral_wolf resolves by key");
    if (wolf) {
        check(npc_type_get(wolf->id) == wolf, "the same type resolves by id");
        check(wolf->faction_index   >= 0, "its faction resolved to an index");
        check(wolf->archetype_index >= 0, "its archetype resolved to an index");
        check(wolf->ability_count   == 1, "it carries one ability");

        const NPCAbilityDefn* a = npc_type_ability(wolf, 0);
        check(a && strcmp(a->key, "lunge_strike") == 0, "that ability is lunge_strike");
        check(a && a->damage == 12, "at the shared row's damage of 12");
    }

    printf("TEST 4: overrides retune a shared row without forking it\n");
    const NPCTypeDef* rogue = npc_type_get_by_key("forgotten_rogue_wolf");
    check(rogue != NULL, "forgotten_rogue_wolf resolves");
    if (rogue && wolf) {
        const NPCAbilityDefn* ra = npc_type_ability(rogue, 0);
        const NPCAbilityDefn* wa = npc_type_ability(wolf, 0);
        check(ra && wa && strcmp(ra->key, wa->key) == 0,
              "both wolves reference the same ability key");
        check(ra && ra->damage == 20, "Rogue Wolf's override raises damage to 20");
        check(wa && wa->damage == 12, "and leaves the shared row untouched at 12");
        check(ra && ra->range_tiles > wa->range_tiles,
              "its range override applied too");
        check(ra && ra->cast_time == wa->cast_time,
              "an un-overridden field still comes from the shared row");
    }

    printf("TEST 5: the pool's sizing inputs are derived, not compiled\n");
    int max_ab = npc_registry_max_abilities();
    check(max_ab > 0, "max_abilities is reported");
    int widest = 0;
    for (int i = 0; i < npc_type_count(); i++) {
        const NPCTypeDef* t = npc_type_at(i);
        if (t && t->ability_count > widest) widest = t->ability_count;
    }
    check(max_ab == widest, "it equals the widest type actually loaded");
    check(npc_registry_max_actions_per_npc() >= 1, "deferred-queue multiplier is sane");

    printf("TEST 6: teardown is clean and repeatable\n");
    npc_registry_cleanup();
    check(npc_type_count() == 0, "cleanup empties the registry");
    check(npc_registry_load(dir) == 1, "and it reloads afterwards");
    npc_registry_cleanup();
    check(npc_type_count() == 0, "and tears down again");

    printf("\n%s: %d failure(s)\n", g_failures ? "FAILED" : "PASSED", g_failures);
    return g_failures ? 1 : 0;
}
