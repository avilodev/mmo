/**
 * @file
 * Check the loaded NPC registry against every rule the content has to satisfy.
 *
 * Separate from the loader on purpose: this runs headless in CI against the shipped
 * data files, so a typo fails a build rather than shipping an enemy that stands
 * still. The loader already rejects unresolvable references; everything here is a
 * rule about *meaning*, which is the class of mistake that otherwise reaches players.
 *
 * The rule worth naming is R7, and it is worth naming twice because it changed.
 *
 * It began as "an ability whose range is shorter than the distance its archetype
 * holds can never fire", which caught a real bug on the first seven types --
 * Risen Soldier's 1-tile swing against a 1.5-tile hold. Once types started
 * carrying mixed kits it began failing content that was correct: a support caster
 * that holds at five tiles and carries a melee swing is not broken, it has an
 * answer for something that closed on it.
 *
 * The fix was in the engine rather than the rule. A profile's hold distance is
 * now clamped to the longest reach its kit actually has (npc_ai_profile.c), so an
 * enemy can no longer stand outside its own range -- the failure R7 existed to
 * catch is unrepresentable. What remains here is the part that is still checkable
 * and still silent: a type with no usable ability at all.
 */

#include "npc_registry_internal.h"
#include "log.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/** Count and report one validation failure. */
static int g_fail_count;

static void bad(const char* type_key, const char* rule, const char* fmt, ...) {
    char detail[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(detail, sizeof(detail), fmt, ap);
    va_end(ap);
    if (g_fail_count < NPC_VALIDATE_REPORT_MAX)
        LOG_ERROR("[NPC_CONTENT] %s  type '%s': %s", rule, type_key, detail);
    g_fail_count++;
}

/** Report whether an id falls inside an inclusive band. */
static int in_band(int id, const int band[2]) {
    return id >= band[0] && id <= band[1];
}

/** R4: ids are unique and inside the band their kind declares. */
static void check_ids(void) {
    for (int i = 0; i < g_npcreg_type_count; i++) {
        const NPCTypeDef* t = &g_npcreg_types[i];

        for (int j = i + 1; j < g_npcreg_type_count; j++)
            if (g_npcreg_types[j].id == t->id)
                bad(t->key, "R4", "shares id %u with '%s'", t->id, g_npcreg_types[j].key);

        const int* band = t->summon_only ? g_npcreg_band_summon : g_npcreg_band_enemy;
        if (!in_band(t->id, band))
            bad(t->key, "R4", "id %u is outside the %s band [%d..%d]",
                t->id, t->summon_only ? "summon" : "enemy", band[0], band[1]);

        if (in_band(t->id, g_npcreg_band_quest))
            bad(t->key, "R5", "id %u collides with the quest/system band [%d..%d]",
                t->id, g_npcreg_band_quest[0], g_npcreg_band_quest[1]);
    }
}

/** R6: an ability is well formed for the delivery it declares. */
static void check_ability_shape(const NPCTypeDef* t, const NPCAbilityDefn* a) {
    if (a->cooldown < 0)
        bad(t->key, "R6", "'%s' has a negative cooldown", a->key);

    switch (a->delivery) {
        case NPC_DELIV_PROJECTILE:
            if (!a->has_projectile)
                bad(t->key, "R6", "'%s' is a projectile with no projectile block", a->key);
            else if (a->projectile.speed <= 0)
                bad(t->key, "R6", "'%s' has a projectile speed of %.1f", a->key,
                    (double)a->projectile.speed);
            if (a->range_tiles <= 0)
                bad(t->key, "R6", "'%s' is a projectile with no range", a->key);
            break;
        case NPC_DELIV_TELEGRAPH:
            if (!a->has_telegraph)
                bad(t->key, "R6", "'%s' is a telegraph with no telegraph block", a->key);
            if (a->range_tiles <= 0)
                bad(t->key, "R6", "'%s' is a telegraph with no range", a->key);
            break;
        case NPC_DELIV_SUMMON:
            if (!a->has_summon)
                bad(t->key, "R6", "'%s' is a summon with no summon block", a->key);
            break;
        case NPC_DELIV_BUFF:
            if (!a->has_buff && !a->has_heal)
                bad(t->key, "R6", "'%s' is a buff with neither buff nor heal", a->key);
            break;
        case NPC_DELIV_DISPLACE:
            if (!a->has_displace)
                bad(t->key, "R6", "'%s' displaces with no displace block", a->key);
            break;
        default:
            break;
    }
}

/** Report whether an ability needs no target and so is castable from anywhere. */
static int is_self_directed(const NPCAbilityDefn* a) {
    return a->delivery == NPC_DELIV_PASSIVE ||
           a->delivery == NPC_DELIV_BUFF ||
           a->delivery == NPC_DELIV_SUMMON ||
           (a->delivery == NPC_DELIV_ZONE && a->range_tiles <= 0.0f);
}

/** R7: a type has at least one ability it can actually use.
 *
 * A fightable enemy with nothing usable stands there and is killed, which reads
 * as an aggro bug rather than as content. That is still silent by construction,
 * so it is still checked -- but the narrower "reaches less far than it stands"
 * check it replaced now belongs to the engine, which clamps a profile's hold
 * distance to its kit's reach. See this file's header.
 */
static void check_reachability(const NPCTypeDef* t, const NPCArchetypeDef* arch) {
    (void)arch;
    if (t->summon_only) return;   /* Decoys have no abilities on purpose. */

    int usable = 0;
    for (int slot = 0; slot < t->ability_count; slot++) {
        const NPCAbilityDefn* a = npc_type_ability(t, slot);
        if (!a) continue;
        if (is_self_directed(a) || a->range_tiles > 0.0f) { usable = 1; break; }
    }

    if (!usable && t->ability_count > 0)
        bad(t->key, "R7", "carries %d ability/abilities and can use none of them",
            t->ability_count);
    if (t->ability_count == 0 && t->health > 0)
        bad(t->key, "R7", "is a fightable enemy with no abilities at all");
}

/** R8/R9: summon graphs terminate and their targets are marked summon-only. */
static void check_summon(const NPCTypeDef* t, const NPCAbilityDefn* a, int depth) {
    if (!a->has_summon) return;

    for (int i = 0; i < a->summon.type_count; i++) {
        int idx = a->summon.type_index[i];
        if (idx < 0 || idx >= g_npcreg_type_count) {
            bad(t->key, "R9", "'%s' summon target %d did not resolve", a->key, i);
            continue;
        }
        const NPCTypeDef* target = &g_npcreg_types[idx];

        if (target == t)
            bad(t->key, "R8", "'%s' summons its own type", a->key);

        if (depth > 4) {
            bad(t->key, "R8", "'%s' starts a summon chain deeper than 4 -- it may not terminate",
                a->key);
            return;
        }
        for (int s = 0; s < target->ability_count; s++)
            check_summon(target, npc_type_ability(target, s), depth + 1);
    }
}

/** R10: trigger values are in range and repeating triggers say so. */
static void check_triggers(const NPCTypeDef* t) {
    for (int i = 0; i < t->trigger_count; i++) {
        const NPCTriggerDef* g = npc_registry_trigger(t->trigger_first + i);
        if (!g) continue;

        if (g->when == NPC_WHEN_HP_BELOW && (g->value <= 0 || g->value >= 100))
            bad(t->key, "R10", "hp_below trigger threshold %.1f is not inside (0, 100)",
                (double)g->value);

        if (g->when == NPC_WHEN_TIMER) {
            if (g->value <= 0)
                bad(t->key, "R10", "timer trigger has a non-positive period");
            if (!g->repeating && !g->once)
                bad(t->key, "R10", "timer trigger is neither repeating nor once");
        }
        if (g->action == NPC_DO_CAST && g->ability_index < 0)
            bad(t->key, "R10", "trigger casts but names no ability");
        if (g->action == NPC_DO_SWAP_ABILITY && (g->from_index < 0 || g->to_index < 0))
            bad(t->key, "R10", "swap_ability trigger is missing its from or to");
    }
}

/** R11: an affix is only placed on a type it declares itself applicable to. */
static void check_affixes(const NPCTypeDef* t) {
    for (int i = 0; i < t->affix_count; i++) {
        int idx = g_npcreg_affix_refs[t->affix_first + i];
        const NPCAffixDef* a = npc_affix_at(idx);
        if (!a) continue;

        if (a->role_mask && !(a->role_mask & (1u << t->role)))
            bad(t->key, "R11", "affix '%s' does not apply to role '%s'",
                a->key, g_npcreg_role_names[t->role]);

        if (a->faction_mask && t->faction_index >= 0 &&
            !(a->faction_mask & (1u << t->faction_index)))
            bad(t->key, "R11", "affix '%s' does not apply to faction '%s'",
                a->key, g_npcreg_factions[t->faction_index].key);
    }
}

/** R11a: phases partition the kit -- nothing is locked out, nothing is unreachable. */
static void check_phases(const NPCTypeDef* t) {
    if (t->phase_count == 0) return;

    for (int slot = 0; slot < t->ability_count; slot++) {
        int reachable = 0;
        for (int p = 0; p < t->phase_count && !reachable; p++) {
            const NPCPhaseDef* ph = npc_registry_phase(t->phase_first + p);
            if (!ph) continue;
            for (int k = 0; k < ph->slot_count; k++)
                if (npc_registry_phase_slot(ph->slot_first + k) == slot) { reachable = 1; break; }
        }
        if (!reachable) {
            const NPCAbilityDefn* a = npc_type_ability(t, slot);
            bad(t->key, "R11a", "'%s' is in no phase, so it can never be used",
                a ? a->key : "(slot)");
        }
    }
    for (int p = 0; p < t->phase_count; p++) {
        const NPCPhaseDef* ph = npc_registry_phase(t->phase_first + p);
        if (ph && ph->slot_count == 0)
            bad(t->key, "R11a", "phase '%s' allows no abilities", ph->key);
    }
}

/** R12/R13/R14: rewards, summon marking, and a speed a melee enemy can catch a player at. */
static void check_type_meta(const NPCTypeDef* t, const NPCArchetypeDef* arch) {
    if (t->loot_table[0] == '\0')
        bad(t->key, "R12", "has no loot table and does not say \"none\"");

    if (t->summon_only && t->xp_reward != 0)
        bad(t->key, "R13", "is summon-only but awards %d XP", t->xp_reward);

    if (!t->summon_only && t->xp_reward <= 0 && t->health > 0)
        bad(t->key, "R12", "is a fightable enemy that awards no XP");

    if (t->health <= 0)
        bad(t->key, "R12", "has no health");

    float speed = t->move_speed > 0 ? t->move_speed : arch->move_speed;

    if (arch->movement != NPC_ARCH_STATIONARY && speed <= 0)
        bad(t->key, "R14", "archetype '%s' moves but its speed is %.0f",
            arch->key, (double)speed);

    /* A level-1 player runs at 195-245. A chaser below ~150 never lands a hit on
     * anyone who keeps walking, which is indistinguishable from broken AI --
     * unless it never gives up, which is what `relentless` means. Husk and Rot
     * Crawler are *designed* to be outrun; being slow and unshakeable is the
     * threat, and outrunning one is the counterplay rather than the bug. */
    if (arch->movement == NPC_ARCH_FOLLOW && arch->preferred_tiles <= 2.0f &&
        !arch->never_leashes && speed > 0 && speed < 150)
        bad(t->key, "R14",
            "closes to melee at %.0f units/sec, below the ~150 a moving player outruns",
            (double)speed);
}

/** R16: every effect the content names is implemented in this build. */
static void check_effects(const NPCTypeDef* t, const NPCAbilityDefn* a) {
    int spans[3][2] = {
        { a->effect_first,             a->effect_count },
        { a->zone_impact.effect_first, a->has_zone_impact ? a->zone_impact.effect_count : 0 },
        { a->buff.effect_first,        a->has_buff ? a->buff.effect_count : 0 },
    };
    for (int s = 0; s < 3; s++) {
        for (int i = 0; i < spans[s][1]; i++) {
            const AbilityEffectDef* e = npc_registry_effect(spans[s][0] + i);
            if (!e) continue;
            if (e->type <= EFFECT_NONE || e->type >= EFFECT_COUNT)
                bad(t->key, "R16", "'%s' applies an effect this build does not implement",
                    a->key);
        }
    }
}

int npc_content_validate(void) {
    g_fail_count = 0;

    if (g_npcreg_type_count == 0) {
        LOG_ERROR("[NPC_CONTENT] the registry holds no types; an empty world loads "
                  "'successfully' and is the failure this check exists to catch");
        return 1;
    }

    check_ids();

    for (int i = 0; i < g_npcreg_type_count; i++) {
        const NPCTypeDef* t = &g_npcreg_types[i];
        const NPCArchetypeDef* arch = npc_archetype_at(t->archetype_index);
        if (!arch) { bad(t->key, "R2", "archetype did not resolve"); continue; }

        if (t->ability_count == 0 && !t->summon_only && t->health > 0)
            bad(t->key, "R6", "has no abilities, so it will stand and be hit");

        for (int s = 0; s < t->ability_count; s++) {
            const NPCAbilityDefn* a = npc_type_ability(t, s);
            if (!a) continue;
            check_ability_shape(t, a);

            check_summon(t, a, 0);
            check_effects(t, a);
        }

        check_triggers(t);
        check_affixes(t);
        check_phases(t);
        check_type_meta(t, arch);
        check_reachability(t, arch);
    }

    if (g_fail_count > NPC_VALIDATE_REPORT_MAX)
        LOG_ERROR("[NPC_CONTENT] %d further problems not listed",
                  g_fail_count - NPC_VALIDATE_REPORT_MAX);

    if (g_fail_count == 0)
        LOG_INFO("[NPC_CONTENT] %d types validated", g_npcreg_type_count);

    return g_fail_count;
}

int npc_content_check_capacities(int max_npcs, int npc_effect_slots) {
    int problems = 0;

    /* R15: a world configured smaller than its own content fails here, at startup,
     * rather than at the spawn that overflows it. */
    if (max_npcs > 0 && max_npcs < g_npcreg_type_count) {
        LOG_WARN("[NPC_CONTENT] max_npcs is %d but the registry holds %d types; a world "
                 "cannot hold one of each", max_npcs, g_npcreg_type_count);
        problems++;
    }
    if (npc_effect_slots > 0 && npc_effect_slots < 1) problems++;

    return problems;
}
