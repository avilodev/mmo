#ifndef RACE_REGISTRY_H
#define RACE_REGISTRY_H

/** @file Load the race registry from data and answer questions about it.
 *
 * Race and class fuse into one identifier under the Blessed model: a character's
 * class_id and race_id always hold the same value, and this registry is the single
 * source of truth for what that value means. Names, passives, specs, ability keys
 * and stat growth all come from races.json.
 *
 * Adding a race is therefore one more block in races.json plus its abilities —
 * no code change, no recompile, no protocol bump, no database migration.
 */

#include "protocol.h"

#include <stdint.h>

/** Bound the fixed-width text and per-race collections. */
#define MAX_RACE_KEY       32
#define MAX_RACE_NAME      32
#define MAX_RACE_LATIN     32
#define MAX_PASSIVE_DESC  192
#define MAX_SPECS_PER_RACE  4
#define MAX_SPEC_ID         8

/** Bound the modifiers one passive may carry. */
#define MAX_PASSIVE_MODIFIERS 4

/** Identify what a passive modifier changes. */
typedef enum {
    PASSIVE_MOD_DAMAGE_TAKEN = 0,  /**< Fraction; 0.10 means 10% less damage taken. */
    PASSIVE_MOD_DAMAGE_DEALT,      /**< Fraction; 0.10 means 10% more damage dealt. */
    PASSIVE_MOD_ARMOR,             /**< Flat armor. */
    PASSIVE_MOD_MOVE_SPEED,        /**< Flat world pixels per second. */
    PASSIVE_MOD_COUNT
} PassiveModifierKind;

/** Describe one mechanical part of a passive.
 *
 * A passive is a base value plus an optional term that scales with the number of
 * nearby allies, which between them express both shapes the designed races need:
 * Thick Hide is a flat 10% reduction, Pack Sense is entirely per-ally. Adding a
 * passive to a new race is therefore data, like everything else about the race.
 */
typedef struct {
    PassiveModifierKind kind;
    double value;       /**< Applied unconditionally. */
    double per_ally;    /**< Added once per nearby ally. */
    int    max_allies;  /**< Cap on how many allies count; 0 disables the per-ally term. */
} PassiveModifier;

/** Describe a race's always-on trait, which resolves in Animal Form only.
 *
 * Human Form applies no passive at all, for any race. That is what makes Human Form
 * literally identical across races rather than merely intended to be.
 */
typedef struct {
    char key[MAX_RACE_KEY];
    char name[MAX_RACE_NAME];
    char description[MAX_PASSIVE_DESC];
    PassiveModifier modifiers[MAX_PASSIVE_MODIFIERS];
    int  modifier_count;
} RacePassive;

/** Describe one of a race's specialisations and the kit it fills the hotbar with. */
typedef struct {
    char       id[MAX_SPEC_ID];     /**< "a" or "b" as written in races.json. */
    CombatRole role;
    uint8_t    is_default;          /**< Nonzero for the spec a new character starts in. */
    uint8_t    unlock_level;        /**< Level required before the spec may be chosen. */
    /** Hold ability keys resolved against abilities.json at ability-load time. */
    char       ability_keys[MAX_ABILITY_SLOTS][MAX_RACE_KEY];
    uint8_t    ability_count;
} RaceSpec;

/** Aggregate one race's identity, passive, specs, and stat curve. */
typedef struct {
    uint32_t    id;                 /**< Fused race/class identifier; 1-based. */
    char        key[MAX_RACE_KEY];  /**< Stable machine name, e.g. "wolf". */
    char        name[MAX_RACE_NAME];
    char        latin[MAX_RACE_LATIN];
    uint8_t     playable;           /**< Nonzero when the race may be chosen at creation. */

    RacePassive passive;

    RaceSpec    specs[MAX_SPECS_PER_RACE];
    uint8_t     spec_count;
    uint8_t     default_spec;       /**< Index into specs[] of the starting spec. */

    /** Hold attributes at level one, in whole points, indexed by StatId. */
    int         base_stats[STAT_COUNT];
    /** Hold growth per level in tenths of a point, indexed by StatId.
     *
     * Tenths keep fractional growth exact without floats, matching the convention
     * the compiled class table used before this registry replaced it. */
    int         per_level_stats[STAT_COUNT];

    float       base_move_speed;    /**< World pixels per second before Dexterity. */
} RaceDef;

/** Load races.json into the registry, replacing any previous contents.
 *
 * @param json_filepath  Path to races.json.
 * @return               The number of races loaded, or 0 on failure.
 */
int race_registry_init(const char* json_filepath);

/** Release the registry. Safe to call when nothing was loaded. */
void race_registry_cleanup(void);

/** Return how many races are loaded. This is the runtime race count; nothing hardcodes it. */
int race_registry_count(void);

/** Look up a race by its fused identifier.
 *
 * @return A registry-owned definition, or NULL for an unknown identifier.
 */
const RaceDef* race_get(uint32_t race_id);

/** Look up a race by its stable machine name.
 *
 * @return A registry-owned definition, or NULL for an unknown key.
 */
const RaceDef* race_get_by_key(const char* key);

/** Return the race at `index` in load order, for enumeration.
 *
 * @return A registry-owned definition, or NULL when out of range.
 */
const RaceDef* race_at(int index);

/** Report whether an untrusted identifier names a race a character may be created as.
 *
 * This is the check the realm server owes every create request: it rejects both
 * out-of-range identifiers and races that exist but carry no designed kit.
 *
 * @return 1 when the race is loaded and playable, or 0 otherwise.
 */
int race_is_playable(uint32_t race_id);

/** Return a race's spec by index, or NULL when out of range. */
const RaceSpec* race_spec_at(const RaceDef* race, int index);

/** Return the spec a character of this race uses by default.
 *
 * @return A registry-owned spec, or NULL when the race has none.
 */
const RaceSpec* race_default_spec(const RaceDef* race);

/** Return the resource a role spends.
 *
 * Resource follows role, not race, so it is derived here rather than stored per race.
 */
ResourceType role_resource(CombatRole role);

/** Return the stat that sizes a role's resource pool. */
StatId role_resource_stat(CombatRole role);

/** Return the lowercase name of a role, for logging and client display. */
const char* role_name(CombatRole role);

/** Return the total value of one passive modifier kind for a given ally count.
 *
 * @param passive     The race's passive; may be NULL, which contributes nothing.
 * @param kind        Which modifier to total.
 * @param ally_count  Nearby allies, clamped to each modifier's own cap.
 * @return            The summed value, or zero when the passive has no such modifier.
 */
double race_passive_modifier(const RacePassive* passive, PassiveModifierKind kind,
                             int ally_count);

/** Return the canonical JSON key of a stat, or NULL for an out-of-range index.
 *
 * These keys are the contract between StatId and races.json. Adding a stat is one
 * enum entry, one entry in this table, and one key per race.
 */
const char* stat_key(StatId stat);

/** Resolve a stat's JSON key to its index.
 *
 * @return The StatId, or -1 when the key names no stat.
 */
int stat_from_key(const char* key);

/** Compute a race's attribute at a level, folding in the per-level tenths.
 *
 * @return The attribute in whole points, or 0 for an unknown race or stat.
 */
int race_stat_at_level(const RaceDef* race, StatId stat, int level);

#endif // RACE_REGISTRY_H
