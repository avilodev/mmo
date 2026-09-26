#ifndef NPC_REGISTRY_INTERNAL_H
#define NPC_REGISTRY_INTERNAL_H

/** @file Share the registry's storage between its loading and lookup halves.
 *
 * npc_registry.c owns the tables, the public lookups and teardown; npc_registry_load.c
 * owns the parsing. They are one module split at the seam where it crossed the
 * 800-line ceiling, not two modules -- nothing outside those two files may include
 * this header, and the tables are const to everything that does.
 *
 * Mirrors the arrangement common/include/net_reactor_internal.h already uses.
 */

#include "npc_registry.h"
#include "json_util.h"

#include <stddef.h>

/* --- Shared storage ------------------------------------------------------ */

extern NPCFactionDef*   g_npcreg_factions;     extern int g_npcreg_faction_count;
extern NPCArchetypeDef* g_npcreg_archetypes;   extern int g_npcreg_archetype_count;
extern NPCAbilityDefn*  g_npcreg_abilities;    extern int g_npcreg_ability_count;
extern NPCAffixDef*     g_npcreg_affixes;      extern int g_npcreg_affix_count;
extern NPCTypeDef*      g_npcreg_types;        extern int g_npcreg_type_count;

extern NPCAbilityDefn*  g_npcreg_type_abilities;
extern int g_npcreg_type_ability_count, g_npcreg_type_ability_cap;

extern AbilityEffectDef* g_npcreg_effects;   extern int g_npcreg_effect_count,  g_npcreg_effect_cap;
extern NPCTriggerDef*    g_npcreg_triggers;  extern int g_npcreg_trigger_count, g_npcreg_trigger_cap;
extern NPCPhaseDef*      g_npcreg_phases;    extern int g_npcreg_phase_count,   g_npcreg_phase_cap;
extern int*              g_npcreg_phase_slots;
extern int g_npcreg_phase_slot_count, g_npcreg_phase_slot_cap;
extern int*              g_npcreg_affix_refs;
extern int g_npcreg_affix_ref_count, g_npcreg_affix_ref_cap;

/** Hold summon targets as keys until types exist. Parallel to g_npcreg_abilities. */
typedef struct {
    char keys[NPC_SUMMON_TYPES_MAX][NPC_KEY_MAX];
    int  count;
} NPCSummonKeys;
extern NPCSummonKeys* g_npcreg_summon_keys;

/** The same, per resolved type-ability instance. Parallel to g_npcreg_type_abilities.
 *
 * Five summoners share one `summon_adds` row and each calls something different:
 * a Packmaster brings wolves and a Grave Caller brings the dead. What they bring
 * is therefore a per-type override, which needs somewhere per-instance to hold
 * the keys until the type table exists to resolve them against. */
extern NPCSummonKeys* g_npcreg_type_summon_keys;
extern int g_npcreg_type_summon_cap;

/** Identifier bands, read from types.json's `id_ranges` header rather than compiled.
 *
 * Widening a band is a content edit. [0] is the low bound, [1] the high, inclusive. */
extern int g_npcreg_band_quest[2];
extern int g_npcreg_band_enemy[2];
extern int g_npcreg_band_summon[2];

extern int g_npcreg_errors;
extern int g_npcreg_max_abilities, g_npcreg_max_triggers, g_npcreg_max_phases;

/* --- Shared helpers ------------------------------------------------------ */

/** Report one content failure against the file and row it came from. */
void npcreg_fail(const char* file, const char* key, const char* fmt, ...);

/** Copy a JSON string into a fixed-width field, always terminating. */
void npcreg_copy_key(char* dst, size_t cap, const JsonValue* obj, const char* key,
                     const char* fallback);

/** Grow a dynamic pool to hold one more element. Returns 0 on allocation failure. */
int npcreg_pool_reserve(void** items, int* cap, int count, size_t elem);

#define NPCREG_RESERVE(arr, cap, count) \
    npcreg_pool_reserve((void**)&(arr), &(cap), (count), sizeof(*(arr)))

/** Match a name against a table, returning its index or `fallback`. */
int npcreg_lookup_name(const char* s, const char* const* names, int n, int fallback);

/** Name tables shared by the loader and the validator's diagnostics. */
extern const char* const g_npcreg_delivery_names[];
extern const char* const g_npcreg_movement_names[];
extern const char* const g_npcreg_priority_names[];
extern const char* const g_npcreg_role_names[];
extern const char* const g_npcreg_shape_names[];
extern const char* const g_npcreg_resolve_names[];
extern const char* const g_npcreg_when_names[];
extern const char* const g_npcreg_action_names[];
extern const char* const g_npcreg_break_names[];
extern const char* const g_npcreg_dmgtype_names[];

/** Open one content file and locate its top-level array member.
 *
 * @param out_array  Receives the array; the caller frees the returned root.
 * @return           The parsed root, or NULL when the file is missing or malformed.
 */
JsonValue* npcreg_open_file(const char* dir, const char* name, const char* member,
                            const JsonValue** out_array);

/** Parse one trigger row into the shared trigger pool. Returns 1 when stored.
 *
 * @param owner  The type whose slots `swap_ability` and `set_phase` resolve
 *               against, or NULL for an affix, whose triggers may reference
 *               neither.
 */
int npcreg_parse_trigger(const JsonValue* o, const NPCTypeDef* owner,
                         const char* file, const char* key);

/** Parse types.json, composing every reference it makes. Returns 1 on success. */
int npcreg_load_types(const char* data_dir);

/** Resolve summon target keys to type indices, once both tables exist. */
void npcreg_resolve_summons(void);

/** Parse every content file in order. Returns 1 when all five parsed. */
int npcreg_load_all(const char* data_dir);

#endif // NPC_REGISTRY_INTERNAL_H
