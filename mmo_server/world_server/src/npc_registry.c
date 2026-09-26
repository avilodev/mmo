/**
 * @file
 * Own the registry's tables, its lookups, and its teardown.
 *
 * Every table here is heap-allocated to what the files actually contain. There is
 * deliberately no compiled cap on types, abilities, effects, triggers or phases:
 * the format this replaces dropped content past MAX_NPC_AI_PROFILES with a log line
 * that read, in game, as an enemy that would not aggro.
 *
 * Parsing lives in npc_registry_load.c, which shares this file's storage through
 * npc_registry_internal.h. The split is at the 800-line ceiling, not at a seam
 * between concerns: npcreg_load_all() is the only thing that crosses it.
 */

#include "npc_registry_internal.h"
#include "json_util.h"
#include "log.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- Storage ------------------------------------------------------------- */

NPCFactionDef*   g_npcreg_factions;     int g_npcreg_faction_count;
NPCArchetypeDef* g_npcreg_archetypes;   int g_npcreg_archetype_count;
NPCAbilityDefn*  g_npcreg_abilities;    int g_npcreg_ability_count;
NPCAffixDef*     g_npcreg_affixes;      int g_npcreg_affix_count;
NPCTypeDef*      g_npcreg_types;        int g_npcreg_type_count;

/** Resolved per-type ability instances: a base row with this type's overrides applied. */
NPCAbilityDefn* g_npcreg_type_abilities; int g_npcreg_type_ability_count, g_npcreg_type_ability_cap;

AbilityEffectDef* g_npcreg_effects;   int g_npcreg_effect_count,  g_npcreg_effect_cap;
NPCTriggerDef*    g_npcreg_triggers;  int g_npcreg_trigger_count, g_npcreg_trigger_cap;
NPCPhaseDef*      g_npcreg_phases;    int g_npcreg_phase_count,   g_npcreg_phase_cap;
int*              g_npcreg_phase_slots; int g_npcreg_phase_slot_count, g_npcreg_phase_slot_cap;
int*              g_npcreg_affix_refs;  int g_npcreg_affix_ref_count,  g_npcreg_affix_ref_cap;

NPCSummonKeys* g_npcreg_summon_keys;
NPCSummonKeys* g_npcreg_type_summon_keys;  int g_npcreg_type_summon_cap;

static int g_loaded;
int g_npcreg_max_abilities, g_npcreg_max_triggers, g_npcreg_max_phases;

/** Count load failures so every problem is reported, not just the first. */
int g_npcreg_band_quest[2]  = { 20, 99 };
int g_npcreg_band_enemy[2]  = { 100, 199 };
int g_npcreg_band_summon[2] = { 200, 255 };

int g_npcreg_errors;

/** Report one load failure against the file and row it came from. */
void npcreg_fail(const char* file, const char* key, const char* fmt, ...) {
    char detail[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(detail, sizeof(detail), fmt, ap);
    va_end(ap);
    if (g_npcreg_errors < NPC_VALIDATE_REPORT_MAX)
        LOG_ERROR("[NPC_REGISTRY] %s: '%s': %s", file, key ? key : "(row)", detail);
    g_npcreg_errors++;
}

/* --- Small helpers ------------------------------------------------------- */

/** Copy a JSON string into a fixed-width field, always terminating. */
void npcreg_copy_key(char* dst, size_t cap, const JsonValue* obj, const char* key,
                     const char* fallback) {
    const char* src = json_get_string(obj, key, fallback);
    size_t len = strlen(src);
    if (len >= cap) len = cap - 1;
    memcpy(dst, src, len);
    dst[len] = '\0';
}

/** Grow a dynamic pool to hold one more element. Returns 0 on allocation failure. */
int npcreg_pool_reserve(void** items, int* cap, int count, size_t elem) {
    if (count < *cap) return 1;
    int next = *cap ? *cap * 2 : 16;
    void* grown = realloc(*items, (size_t)next * elem);
    if (!grown) return 0;
    *items = grown;
    *cap = next;
    return 1;
}


/* --- Enum parsing -------------------------------------------------------- */

/** Match a name against a table, returning its index or `fallback`. */
int npcreg_lookup_name(const char* s, const char* const* names, int n, int fallback) {
    if (!s) return fallback;
    for (int i = 0; i < n; i++)
        if (strcmp(s, names[i]) == 0) return i;
    return fallback;
}

const char* const g_npcreg_delivery_names[] = {
    "contact", "projectile", "telegraph", "zone", "summon", "buff", "displace", "passive"
};
const char* const g_npcreg_movement_names[]  = { "stationary", "follow", "maintain_range" };
const char* const g_npcreg_priority_names[]  = { "nearest", "blessed_first", "cluster", "lowest_hp" };
const char* const g_npcreg_role_names[]      = { "melee", "ranged", "brute", "support",
                                           "summoner", "add", "elite", "miniboss" };
const char* const g_npcreg_shape_names[]     = { "circle", "cone", "rectangle", "line" };
const char* const g_npcreg_resolve_names[]   = { "damage", "none", "random_range", "random_cast_time" };
const char* const g_npcreg_when_names[]      = { "hp_below", "timer", "incoming_damage",
                                           "ability_cast_nearby", "on_death", "proximity",
                                           "allies_below", "on_kill", "ability_missed" };
const char* const g_npcreg_action_names[]    = { "modify", "cast", "swap_ability", "summon",
                                           "flee", "set_phase" };
const char* const g_npcreg_break_names[]     = { "none", "hits_in_window", "parry_frame",
                                           "shield_pool", "effect", "timed" };
const char* const g_npcreg_dmgtype_names[]   = { "physical", "earth", "spirit" };


static void compute_maxima(void) {
    g_npcreg_max_abilities = g_npcreg_max_triggers = g_npcreg_max_phases = 0;
    for (int i = 0; i < g_npcreg_type_count; i++) {
        const NPCTypeDef* t = &g_npcreg_types[i];
        if (t->ability_count > g_npcreg_max_abilities) g_npcreg_max_abilities = t->ability_count;
        if (t->phase_count   > g_npcreg_max_phases)    g_npcreg_max_phases    = t->phase_count;

        /* A type's latch bitset has to cover its affixes' triggers as well as
         * its own: at runtime the two are one concatenated list, and sizing to
         * the type's own count alone would silently drop every affix trigger
         * past the end -- an affix that composes on and then does nothing. */
        int triggers = t->trigger_count;
        for (int a = 0; a < t->affix_count; a++) {
            int ai = g_npcreg_affix_refs[t->affix_first + a];
            if (ai >= 0 && ai < g_npcreg_affix_count)
                triggers += g_npcreg_affixes[ai].trigger_count;
        }
        if (triggers > g_npcreg_max_triggers) g_npcreg_max_triggers = triggers;
    }

    /* An unaffixed world still has to leave room for one, because affixes are
     * composed at spawn: a type with no affix listed can still be given one. */
    for (int i = 0; i < g_npcreg_affix_count; i++)
        if (g_npcreg_affixes[i].trigger_count > 0 &&
            g_npcreg_max_triggers < g_npcreg_affixes[i].trigger_count)
            g_npcreg_max_triggers = g_npcreg_affixes[i].trigger_count;
}

/* --- Public API ---------------------------------------------------------- */

int npc_registry_load(const char* data_dir) {
    npc_registry_cleanup();
    g_npcreg_errors = 0;

    if (!data_dir || !data_dir[0]) {
        LOG_ERROR("[NPC_REGISTRY] no data directory given");
        return 0;
    }

    int ok = npcreg_load_all(data_dir);

    if (!ok || g_npcreg_errors > 0) {
        LOG_ERROR("[NPC_REGISTRY] refusing to load: %d problem(s) in %s",
                  g_npcreg_errors, data_dir);
        npc_registry_cleanup();
        return 0;
    }

    compute_maxima();
    g_loaded = 1;

    LOG_INFO("[NPC_REGISTRY] %d types, %d abilities, %d archetypes, %d factions, "
             "%d affixes; widest type has %d abilities, %d triggers, %d phases",
             g_npcreg_type_count, g_npcreg_ability_count, g_npcreg_archetype_count, g_npcreg_faction_count,
             g_npcreg_affix_count, g_npcreg_max_abilities, g_npcreg_max_triggers, g_npcreg_max_phases);
    return 1;
}

void npc_registry_cleanup(void) {
    free(g_npcreg_factions);       g_npcreg_factions = NULL;       g_npcreg_faction_count = 0;
    free(g_npcreg_archetypes);     g_npcreg_archetypes = NULL;     g_npcreg_archetype_count = 0;
    free(g_npcreg_abilities);      g_npcreg_abilities = NULL;      g_npcreg_ability_count = 0;
    free(g_npcreg_affixes);        g_npcreg_affixes = NULL;        g_npcreg_affix_count = 0;
    free(g_npcreg_types);          g_npcreg_types = NULL;          g_npcreg_type_count = 0;
    free(g_npcreg_type_summon_keys); g_npcreg_type_summon_keys = NULL; g_npcreg_type_summon_cap = 0;
    free(g_npcreg_summon_keys);    g_npcreg_summon_keys = NULL;

    free(g_npcreg_type_abilities); g_npcreg_type_abilities = NULL; g_npcreg_type_ability_count = g_npcreg_type_ability_cap = 0;
    free(g_npcreg_effects);        g_npcreg_effects = NULL;        g_npcreg_effect_count = g_npcreg_effect_cap = 0;
    free(g_npcreg_triggers);       g_npcreg_triggers = NULL;       g_npcreg_trigger_count = g_npcreg_trigger_cap = 0;
    free(g_npcreg_phases);         g_npcreg_phases = NULL;         g_npcreg_phase_count = g_npcreg_phase_cap = 0;
    free(g_npcreg_phase_slots);    g_npcreg_phase_slots = NULL;    g_npcreg_phase_slot_count = g_npcreg_phase_slot_cap = 0;
    free(g_npcreg_affix_refs);     g_npcreg_affix_refs = NULL;     g_npcreg_affix_ref_count = g_npcreg_affix_ref_cap = 0;

    g_loaded = 0;
    g_npcreg_max_abilities = g_npcreg_max_triggers = g_npcreg_max_phases = 0;
}

const NPCTypeDef* npc_type_get(uint16_t npc_type_id) {
    for (int i = 0; i < g_npcreg_type_count; i++)
        if (g_npcreg_types[i].id == npc_type_id) return &g_npcreg_types[i];
    return NULL;
}

const NPCTypeDef* npc_type_get_by_key(const char* key) {
    if (!key) return NULL;
    for (int i = 0; i < g_npcreg_type_count; i++)
        if (strcmp(g_npcreg_types[i].key, key) == 0) return &g_npcreg_types[i];
    return NULL;
}

const NPCTypeDef* npc_type_at(int i) {
    return (i >= 0 && i < g_npcreg_type_count) ? &g_npcreg_types[i] : NULL;
}
int npc_type_count(void) { return g_npcreg_type_count; }

const NPCArchetypeDef* npc_archetype_at(int i) {
    return (i >= 0 && i < g_npcreg_archetype_count) ? &g_npcreg_archetypes[i] : NULL;
}
int npc_archetype_count(void) { return g_npcreg_archetype_count; }

const NPCFactionDef* npc_faction_at(int i) {
    return (i >= 0 && i < g_npcreg_faction_count) ? &g_npcreg_factions[i] : NULL;
}
int npc_faction_count(void) { return g_npcreg_faction_count; }

const NPCAbilityDefn* npc_ability_at(int i) {
    return (i >= 0 && i < g_npcreg_ability_count) ? &g_npcreg_abilities[i] : NULL;
}

const NPCAbilityDefn* npc_ability_by_key(const char* key) {
    if (!key || !key[0]) return NULL;
    for (int i = 0; i < g_npcreg_ability_count; i++)
        if (strcmp(g_npcreg_abilities[i].key, key) == 0) return &g_npcreg_abilities[i];
    return NULL;
}
int npc_ability_count(void) { return g_npcreg_ability_count; }

const NPCAffixDef* npc_affix_at(int i) {
    return (i >= 0 && i < g_npcreg_affix_count) ? &g_npcreg_affixes[i] : NULL;
}
int npc_affix_count(void) { return g_npcreg_affix_count; }

const NPCAbilityDefn* npc_type_ability(const NPCTypeDef* t, int slot) {
    if (!t || slot < 0 || slot >= t->ability_count) return NULL;
    return &g_npcreg_type_abilities[t->ability_first + slot];
}

const NPCTriggerDef* npc_registry_trigger(int i) {
    return (i >= 0 && i < g_npcreg_trigger_count) ? &g_npcreg_triggers[i] : NULL;
}
const NPCPhaseDef* npc_registry_phase(int i) {
    return (i >= 0 && i < g_npcreg_phase_count) ? &g_npcreg_phases[i] : NULL;
}
const AbilityEffectDef* npc_registry_effect(int i) {
    return (i >= 0 && i < g_npcreg_effect_count) ? &g_npcreg_effects[i] : NULL;
}
int npc_registry_phase_slot(int i) {
    return (i >= 0 && i < g_npcreg_phase_slot_count) ? g_npcreg_phase_slots[i] : -1;
}

const NPCAffixDef* npc_type_affix(const NPCTypeDef* type, int n) {
    if (!type || n < 0 || n >= type->affix_count) return NULL;
    int idx = g_npcreg_affix_refs[type->affix_first + n];
    if (idx < 0 || idx >= g_npcreg_affix_count) return NULL;
    return &g_npcreg_affixes[idx];
}

int npc_affix_index_by_key(const char* key) {
    if (!key || !key[0]) return -1;
    for (int i = 0; i < g_npcreg_affix_count; i++)
        if (strcmp(g_npcreg_affixes[i].key, key) == 0) return i;
    return -1;
}

int npc_registry_max_abilities(void) { return g_npcreg_max_abilities; }
int npc_registry_max_triggers(void)  { return g_npcreg_max_triggers; }
int npc_registry_max_phases(void)    { return g_npcreg_max_phases; }

int npc_registry_max_actions_per_npc(void) {
    /* The worst honest tick for one NPC: it walks a burst shot, starts or
     * resolves an ability of its own, and a trigger fires and casts a second.
     * A resolve that also lays zones or summons is still one action -- those
     * carry their count inside the action rather than queueing one apiece --
     * so the bound is the number of *decisions*, not the number of entities. */
    return 4;
}

/* --- Accessors the validator needs into private storage ------------------ */

int npc_registry_effect_total(void)       { return g_npcreg_effect_count; }
int npc_registry_type_ability_total(void) { return g_npcreg_type_ability_count; }
int npc_registry_is_loaded(void)          { return g_loaded; }
