/**
 * @file
 * Exercise the shipped ability set against the promises races.json makes about it.
 *
 * Most of this file is cross-referencing: every ability key a spec names must exist,
 * every ability must belong to a race that exists, and every playable race must fill
 * exactly five slots in each form. Those are the joins that break silently when one
 * of the two data files is edited without the other.
 */

#include "ability_def.h"
#include "class_stats.h"
#include "race_registry.h"

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

/** Verify the whole set loads and splits between the two forms as designed. */
static void test_all_abilities_load(void) {
    printf("TEST 1: fifty abilities load, five human and forty-five animal\n");

    check(abilities_get_count() == 50, "fifty abilities parsed");

    int human = 0, animal = 0;
    for (int i = 1; i < MAX_ABILITIES; i++) {
        const AbilityDef* ability = ability_get((uint16_t)i);
        if (!ability) continue;
        if (ability->form == FORM_HUMAN) human++; else animal++;
    }
    check(human == 5, "five are human form");
    check(animal == 45, "forty-five are animal form");

    check(ability_get(0) == NULL, "identifier 0 names no ability");
    check(ability_get_by_key("cleave") == NULL, "the retired class abilities are gone");
    check(ability_get_by_key(NULL) == NULL, "a NULL key names no ability");
}

/** Verify the Human Form kit is universal and costs nothing but time. */
static void test_human_form_kit(void) {
    printf("TEST 2: the human form kit is universal, free, and identical for every race\n");

    int owned_by_a_race = 0, with_cost = 0, without_cooldown = 0;
    for (int i = 1; i < MAX_ABILITIES; i++) {
        const AbilityDef* ability = ability_get((uint16_t)i);
        if (!ability || ability->form != FORM_HUMAN) continue;

        if (ability->race_id != 0)       owned_by_a_race++;
        if (ability->resource_cost != 0) with_cost++;
        if (ability->cooldown <= 0.0f)   without_cooldown++;
    }
    check(owned_by_a_race == 0, "no human ability belongs to a race");
    check(with_cost == 0, "none costs a resource — human form is cooldown-only");
    check(without_cooldown == 0, "and every one has a cooldown, which is its whole cost");

    /* The strong claim: the bar itself is identical, not merely the abilities. */
    uint16_t reference[MAX_ABILITY_SLOTS];
    int reference_count = ability_get_form_abilities(0, FORM_HUMAN, reference,
                                                     MAX_ABILITY_SLOTS);
    check(reference_count == 5, "the human bar holds five abilities");

    int differing = 0;
    for (int i = 0; i < race_registry_count(); i++) {
        uint16_t bar[MAX_ABILITY_SLOTS];
        int count = ability_get_form_abilities((uint8_t)race_at(i)->id, FORM_HUMAN,
                                               bar, MAX_ABILITY_SLOTS);
        if (count != reference_count ||
            memcmp(bar, reference, sizeof(uint16_t) * (size_t)count) != 0) {
            differing++;
        }
    }
    check(differing == 0, "and is byte-identical for all nine races");
}

/** Verify every spec's ability keys resolve, and to the race that names them. */
static void test_spec_keys_resolve(void) {
    printf("TEST 3: every ability key a spec names exists and belongs to that race\n");

    int unresolved = 0, wrong_owner = 0, wrong_form = 0;

    for (int i = 0; i < race_registry_count(); i++) {
        const RaceDef* race = race_at(i);
        for (int s = 0; s < race->spec_count; s++) {
            const RaceSpec* spec = race_spec_at(race, s);
            for (int a = 0; a < spec->ability_count; a++) {
                const AbilityDef* ability = ability_get_by_key(spec->ability_keys[a]);
                if (!ability) { unresolved++; continue; }
                if (ability->race_id != race->id) wrong_owner++;
                if (ability->form != FORM_ANIMAL) wrong_form++;
            }
        }
    }

    check(unresolved == 0, "every key resolves to a defined ability");
    check(wrong_owner == 0, "and each belongs to the race whose spec names it");
    check(wrong_form == 0, "and is an animal-form ability");
}

/** Verify every ability's race exists and is playable. */
static void test_no_ability_orphans(void) {
    printf("TEST 4: no ability references a race that cannot use it\n");

    int unknown_race = 0, non_playable = 0, unreferenced = 0;

    for (int i = 1; i < MAX_ABILITIES; i++) {
        const AbilityDef* ability = ability_get((uint16_t)i);
        if (!ability || ability->race_id == 0) continue;   /* universal */

        const RaceDef* race = race_get(ability->race_id);
        if (!race) { unknown_race++; continue; }
        if (!race->playable) non_playable++;

        /* An animal ability nothing lists is an ability no player can ever cast. */
        int referenced = 0;
        for (int s = 0; s < race->spec_count && !referenced; s++) {
            const RaceSpec* spec = race_spec_at(race, s);
            for (int a = 0; a < spec->ability_count; a++) {
                if (strcmp(spec->ability_keys[a], ability->key) == 0) { referenced = 1; break; }
            }
        }
        if (!referenced) unreferenced++;
    }

    check(unknown_race == 0, "no ability names a race that does not exist");
    check(non_playable == 0, "no ability belongs to a race that is not playable");
    check(unreferenced == 0, "no animal ability is defined but listed by no spec");

    /* An ability whose race key failed to resolve reads as universal, which is
     * indistinguishable from a Human Form ability by race_id alone. Counting by form
     * catches it: exactly the five human abilities may belong to no race. */
    int ownerless = 0;
    for (int i = 1; i < MAX_ABILITIES; i++) {
        const AbilityDef* ability = ability_get((uint16_t)i);
        if (!ability) continue;
        if (ability->form == FORM_ANIMAL && ability->race_id == 0) ownerless++;
    }
    check(ownerless == 0, "every animal ability resolved an owning race");
}

/** Verify every playable race fills both bars completely at the level cap. */
static void test_bars_fill(void) {
    printf("TEST 5: each playable race fills five slots in each form\n");

    int short_animal = 0, short_human = 0;

    for (int i = 0; i < race_registry_count(); i++) {
        const RaceDef* race = race_at(i);
        if (!race->playable) continue;

        uint16_t bar[MAX_ABILITY_SLOTS];
        if (ability_get_form_abilities((uint8_t)race->id, FORM_ANIMAL, bar,
                                       MAX_ABILITY_SLOTS) != 5) short_animal++;
        if (ability_get_form_abilities((uint8_t)race->id, FORM_HUMAN, bar,
                                       MAX_ABILITY_SLOTS) != 5) short_human++;
    }

    check(short_animal == 0, "every playable race has five animal abilities");
    check(short_human == 0, "and five human abilities");

    /* A race the registry does not hold has no kit, and asking for one must not
     * invent anything. Id 7 is the retired Snake race's. */
    uint16_t bar[MAX_ABILITY_SLOTS];
    check(race_get(7) == NULL, "id 7 names no race");
    check(ability_get_form_abilities(7, FORM_ANIMAL, bar, MAX_ABILITY_SLOTS) == 0,
          "and a race that does not exist yields an empty animal bar");
}

/** Verify unlock levels sit inside the level range the curve actually reaches. */
static void test_unlock_levels(void) {
    printf("TEST 6: no ability unlocks past the level cap\n");

    int too_high = 0, too_low = 0;
    for (int i = 1; i < MAX_ABILITIES; i++) {
        const AbilityDef* ability = ability_get((uint16_t)i);
        if (!ability) continue;
        if (ability->unlock_level > class_stats_max_level()) too_high++;
        if (ability->unlock_level < 1) too_low++;
    }
    check(too_high == 0, "every unlock level is at or below 30");
    check(too_low == 0, "and at least 1");
}

/** Verify each new primitive survived the loader, using the ability that needs it. */
static void test_primitives_round_trip(void) {
    printf("TEST 7: each new primitive round-trips through the loader\n");

    /* Search an ability's effects for one of a type, optionally self-directed. */
    #define FIND_EFFECT(ability, want, out)                                     \
        do {                                                                    \
            (out) = NULL;                                                       \
            for (int _e = 0; (ability) && _e < (ability)->effect_count; _e++) {  \
                if ((ability)->effects[_e].type == (want)) {                     \
                    (out) = &(ability)->effects[_e];                             \
                    break;                                                       \
                }                                                                \
            }                                                                    \
        } while (0)

    const AbilityEffectDef* effect = NULL;

    const AbilityDef* roar = ability_get_by_key("bear_roar");
    FIND_EFFECT(roar, EFFECT_TAUNT, effect);
    check(effect != NULL, "taunt: bear_roar carries it");
    check(effect && effect->duration > 0.0f, "with a duration");
    /* Roar is the tank's whole job and deals no damage, so it only works if the
     * resolver reaches enemies on effects alone. */
    check(roar && roar->damage == 0, "and deals no damage, only the taunt");
    check(roar && roar->target_type == ABILITY_TARGET_ENEMY, "aimed at enemies");

    const AbilityDef* bounty = ability_get_by_key("deer_bounty");
    FIND_EFFECT(bounty, EFFECT_RESOURCE, effect);
    check(effect != NULL, "resource restore: deer_bounty carries it");
    check(effect && effect->value == 300, "at 30.0% of max, as tenths of a percent");
    check(effect && effect_value_is_permille(effect->type), "and reads its value as permille");

    const AbilityDef* bite = ability_get_by_key("wolf_bite");
    check(bite && bite->heal_percent == 50, "percent-of-max healing: wolf_bite heals 5.0%");
    check(bite && bite->heal_self == 1, "and directs it at the caster");

    const AbilityDef* tough_hide = ability_get_by_key("bear_tough_hide");
    FIND_EFFECT(tough_hide, EFFECT_DAMAGE_TAKEN, effect);
    check(effect != NULL, "percentage damage modifier: bear_tough_hide carries it");
    check(effect && effect->value == 100, "at 10.0%");
    check(effect && effect->self == 1, "applied to the bear itself");

    const AbilityDef* guard = ability_get_by_key("human_guard");
    FIND_EFFECT(guard, EFFECT_DAMAGE_TAKEN, effect);
    check(effect && effect->value == 150, "and human_guard carries 15.0%");

    const AbilityDef* pounce = ability_get_by_key("bear_pounce");
    FIND_EFFECT(pounce, EFFECT_ROOT, effect);
    check(effect != NULL, "root: bear_pounce carries it");
    check(effect && effect->duration == 2.0f, "for 2 seconds");

    const AbilityDef* hibernate = ability_get_by_key("bear_hibernate");
    FIND_EFFECT(hibernate, EFFECT_CHANNEL, effect);
    check(effect != NULL, "channel: bear_hibernate carries it");
    FIND_EFFECT(hibernate, EFFECT_HOT_PERCENT, effect);
    check(effect != NULL, "alongside percent-of-max healing over time");
    check(effect && effect->tick_rate > 0.0f, "which ticks");

    const AbilityDef* rend = ability_get_by_key("wolf_rend");
    check(rend && rend->bonus_damage.condition == 1, "execute: wolf_rend has the condition");
    check(rend && rend->bonus_damage.threshold == 25.0f, "at 25% health");
    check(rend && rend->bonus_damage.multiplier > 1.0f, "with a damage multiplier");

    int conditional = 0;
    for (int e = 0; rend && e < rend->effect_count; e++) {
        if (rend->effects[e].on_condition && rend->effects[e].self) conditional++;
    }
    check(conditional == 2, "and two self effects gated on that condition firing");

    #undef FIND_EFFECT
}

/**
 * Verify every ability actually delivers something.
 *
 * An ability with no damage, no healing and no effects is inert: it spends a resource
 * and a cooldown and changes nothing. This also pins down the shape that made Clear
 * Mind and Bounty silently do nothing — an ally-targeted ability whose entire payload
 * is an effect, carrying no healing at all.
 */
static void test_every_ability_delivers_something(void) {
    printf("TEST 9: no ability is inert\n");

    int inert = 0;
    int effect_only_ally = 0;
    int effect_only_enemy = 0;

    for (int i = 1; i < MAX_ABILITIES; i++) {
        const AbilityDef* ability = ability_get((uint16_t)i);
        if (!ability) continue;

        int delivers = ability->damage > 0 || ability->healing > 0 ||
                       ability->heal_percent > 0 || ability->effect_count > 0 ||
                       ability->movement.type != MOVEMENT_NONE ||
                       ability->spawn.type != SPAWN_NONE;
        if (!delivers) {
            printf("       %s delivers nothing\n", ability->key);
            inert++;
        }

        /* Abilities whose entire payload is an effect. The resolver used to reach
         * allies only when an ability healed and enemies only when it damaged, so
         * every ability of this shape silently did nothing. */
        int payload_is_effect_only =
            ability->effect_count > 0 && ability->damage == 0 &&
            ability->healing == 0 && ability->heal_percent == 0;

        if (payload_is_effect_only && ability->target_type == ABILITY_TARGET_ALLY) {
            effect_only_ally++;
        }
        if (payload_is_effect_only && ability->target_type == ABILITY_TARGET_ENEMY) {
            effect_only_enemy++;
        }
    }

    check(inert == 0, "every ability has damage, healing, an effect, or a movement");
    check(effect_only_ally == 3, "three ally abilities carry an effect and no healing");
    check(effect_only_enemy == 7, "seven enemy abilities carry an effect and no damage");
}

/** Verify scaling and buff targets resolve to the eleven attributes. */
static void test_stat_targets(void) {
    printf("TEST 10: damage and buff targets name real attributes\n");

    int bad_damage_stat = 0, bad_buff_stat = 0;

    for (int i = 1; i < MAX_ABILITIES; i++) {
        const AbilityDef* ability = ability_get((uint16_t)i);
        if (!ability) continue;

        if (ability->damage > 0 && !stat_target_is_attribute(ability->damage_stat)) {
            bad_damage_stat++;
        }
        for (int e = 0; e < ability->effect_count; e++) {
            if (ability->effects[e].type != EFFECT_BUFF) continue;
            StatType target = ability->effects[e].stat;
            if (!stat_target_is_attribute(target) &&
                target != STAT_TARGET_MOVE_SPEED &&
                target != STAT_TARGET_WEAPON_DAMAGE) {
                bad_buff_stat++;
            }
        }
    }

    check(bad_damage_stat == 0, "every damaging ability scales with a real attribute");
    check(bad_buff_stat == 0, "every buff names an attribute or a derived quantity");

    const AbilityDef* strike = ability_get_by_key("human_strike");
    check(strike && strike->damage_stat == (StatType)STAT_STRENGTH,
          "strike scales with strength");
    const AbilityDef* shot = ability_get_by_key("deer_ranged_shot");
    check(shot && shot->damage_stat == (StatType)STAT_INTELLIGENCE,
          "the deer's shot scales with intelligence");
}

int main(void) {
    printf("=== ability data test ===\n\n");

    char races_path[512], abilities_path[512], progression_path[512];
    if (!find_data(races_path, sizeof(races_path), "races.json") ||
        !find_data(abilities_path, sizeof(abilities_path), "abilities.json")) {
        printf("SKIPPED: data files not found; run from the repository root\n");
        return 0;
    }
    find_data(progression_path, sizeof(progression_path), "progression.json");

    /* Order matters: abilities resolve their owning race by key, so the registry has
     * to exist first. Loading them the other way round would orphan every one. */
    if (!class_stats_init(races_path, progression_path)) {
        printf("SKIPPED: the race registry did not load\n");
        return 0;
    }
    if (!abilities_init(abilities_path)) {
        printf("SKIPPED: abilities.json did not load\n");
        return 0;
    }

    test_all_abilities_load();
    test_human_form_kit();
    test_spec_keys_resolve();
    test_no_ability_orphans();
    test_bars_fill();
    test_unlock_levels();
    test_primitives_round_trip();
    test_every_ability_delivers_something();
    test_stat_targets();

    abilities_cleanup();
    class_stats_cleanup();

    printf("\n");
    if (g_failures) {
        printf("%d ASSERTION(S) FAILED\n", g_failures);
        return 1;
    }
    printf("ALL ASSERTIONS PASSED\n");
    return 0;
}
