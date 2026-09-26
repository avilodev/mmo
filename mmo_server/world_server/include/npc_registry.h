#ifndef NPC_REGISTRY_H
#define NPC_REGISTRY_H

/** @file Own the composed NPC content model: factions, archetypes, abilities, affixes, types.
 *
 * The format this replaces inlined every ability into every type, so a shared attack
 * existed once per user and retuning it was one edit per copy. Here an ability is a
 * named row other rows reference, which is the same thing abilities.json already does
 * for players (`ability_get_by_key`).
 *
 * Three properties this header exists to guarantee:
 *
 *  - **Nothing is sized at compile time.** Every table is heap-allocated to exactly
 *    what the files contain, counted in a first pass. There is no profile cap to
 *    silently drop content past, and no ability cap to overflow -- npc_world.h sizes
 *    its per-NPC arrays from npc_registry_max_abilities() rather than from a #define
 *    that has to be kept in step by hand.
 *
 *  - **Every reference resolves at load or the load fails.** Keys become indices once,
 *    here, and an unresolved key is reported by file, type and field. An enemy that
 *    stands inert because a key was misspelt is the failure this prevents.
 *
 *  - **Distances are authored in tiles.** enemy_types.txt specifies reach in tiles and
 *    the world's tile size is read from world.dat rather than being 16 by definition,
 *    so conversion happens in one place. Fields named *_tiles are unconverted.
 *
 * The registry is immutable once loaded, which is what lets a projectile or a zone
 * carry an (index, count) into the shared effect table instead of copying an array.
 */

#include "ability_def.h"

#include <stdint.h>

/** Bound the fixed-width text fields a content row carries. */
#define NPC_KEY_MAX          40
#define NPC_NAME_MAX         40

/** Bound the summon target list on one ability. Raise freely; it costs one int each. */
#define NPC_SUMMON_TYPES_MAX 4

/* --- Enumerations ------------------------------------------------------- */

/** Select how an ability reaches its target.
 *
 * NPC_DELIV_ rather than npc_ai.h's NPC_DELIVERY_, for the same reason
 * NPCArchMovement does not reuse NPCMovementType: the legacy profile system and
 * this one coexist until npc_ai.c is split, and the two enumerations are not the
 * same set -- this one has six members the old one never had.
 */
typedef enum {
    NPC_DELIV_CONTACT    = 0,  /**< No telegraph; damage on touch. */
    NPC_DELIV_PROJECTILE = 1,
    NPC_DELIV_TELEGRAPH  = 2,
    NPC_DELIV_ZONE       = 3,  /**< Places ground effect, no direct hit. */
    NPC_DELIV_SUMMON     = 4,
    NPC_DELIV_BUFF       = 5,
    NPC_DELIV_DISPLACE   = 6,
    NPC_DELIV_PASSIVE    = 7,  /**< Always-on; never selected by the act phase. */
    NPC_DELIV_COUNT
} NPCDeliveryKind;

/** Position an archetype relative to its target.
 *
 * Deliberately not npc_ai.h's NPCMovementType: the two coexist until npc_ai.c is
 * split, and sharing an enum across that boundary would couple the split to this file.
 */
typedef enum {
    NPC_ARCH_STATIONARY      = 0,
    NPC_ARCH_FOLLOW          = 1,
    NPC_ARCH_MAINTAIN_RANGE  = 2
} NPCArchMovement;

/** Choose which candidate an archetype attacks. */
typedef enum {
    NPC_PRIORITY_NEAREST       = 0,
    NPC_PRIORITY_BLESSED_FIRST = 1,
    NPC_PRIORITY_CLUSTER       = 2,
    NPC_PRIORITY_LOWEST_HP     = 3
} NPCTargetPriority;

/** Client presentation class; picks colour value and box scale. */
typedef enum {
    NPC_ROLE_MELEE    = 0,
    NPC_ROLE_RANGED   = 1,
    NPC_ROLE_BRUTE    = 2,
    NPC_ROLE_SUPPORT  = 3,
    NPC_ROLE_SUMMONER = 4,
    NPC_ROLE_ADD      = 5,
    NPC_ROLE_ELITE    = 6,
    NPC_ROLE_MINIBOSS = 7,
    NPC_ROLE_COUNT
} NPCRole;

/** Client-visible telegraph geometry. Values match npc_ai.h's NPCTelegraphShape. */
typedef enum {
    NPC_TELE_CIRCLE    = 0,
    NPC_TELE_CONE      = 1,
    NPC_TELE_RECTANGLE = 2,
    NPC_TELE_LINE      = 3
} NPCTeleShape;

/** Decide what a telegraph does when its cast completes. */
typedef enum {
    NPC_RESOLVE_DAMAGE           = 0,
    NPC_RESOLVE_NONE             = 1,  /**< A feint. Dasher baits a dodge with this. */
    NPC_RESOLVE_RANDOM_RANGE     = 2,  /**< Grafted Horror: one animation, two reaches. */
    NPC_RESOLVE_RANDOM_CAST_TIME = 3   /**< Failed Experiment. */
} NPCResolveMode;

/** Name the event that fires a trigger. */
typedef enum {
    NPC_WHEN_HP_BELOW            = 0,
    NPC_WHEN_TIMER               = 1,
    NPC_WHEN_INCOMING_DAMAGE     = 2,
    NPC_WHEN_ABILITY_CAST_NEARBY = 3,
    NPC_WHEN_ON_DEATH            = 4,
    NPC_WHEN_PROXIMITY           = 5,
    NPC_WHEN_ALLIES_BELOW        = 6,
    NPC_WHEN_ON_KILL             = 7,
    /** An ability of this NPC's resolved and touched nobody.
     *
     * The one event the source document needs that is not in its own list:
     * Wagon Breaker's Armor Break is "two charges without a hit". Counting
     * whiffs generalises past that one enemy -- every committed attack in the
     * roster is something a player can make miss, and this is the hook for
     * making the miss mean something. */
    NPC_WHEN_ABILITY_MISSED      = 8,
    NPC_WHEN_COUNT
} NPCTriggerWhen;

/** Name what a trigger does when it fires. */
typedef enum {
    NPC_DO_MODIFY       = 0,
    NPC_DO_CAST         = 1,
    NPC_DO_SWAP_ABILITY = 2,
    NPC_DO_SUMMON       = 3,
    NPC_DO_FLEE         = 4,
    NPC_DO_SET_PHASE    = 5,
    NPC_DO_COUNT
} NPCTriggerAction;

/** Decide when a phase yields to the next one. */
typedef enum {
    NPC_PHASE_AFTER         = 0,  /**< Seconds elapsed. */
    NPC_PHASE_AFTER_USES    = 1,  /**< Ability activations inside this phase. */
    NPC_PHASE_TARGET_WITHIN = 2,  /**< Tiles; Overseer cycles on proximity. */
    NPC_PHASE_TARGET_BEYOND = 3
} NPCPhaseExit;

/** Decide how a shield stops being a shield. */
typedef enum {
    NPC_BREAK_NONE           = 0,
    NPC_BREAK_HITS_IN_WINDOW = 1,
    NPC_BREAK_PARRY_FRAME    = 2,
    NPC_BREAK_SHIELD_POOL    = 3,
    NPC_BREAK_EFFECT         = 4,
    NPC_BREAK_TIMED          = 5
} NPCShieldBreak;

/* --- Component definitions ---------------------------------------------- */

/** Describe a telegraph's geometry and what its resolution means. */
typedef struct {
    uint8_t shape;                /**< NPCTeleShape. */
    float   radius_tiles;
    float   angle;                /**< Cone angle, degrees. */
    float   width_tiles;
    float   length_tiles;
    uint8_t at_target;            /**< Centre on the target's position, not the caster. */
    uint8_t teleport_on_resolve;
    uint8_t resolve_mode;         /**< NPCResolveMode. */
} NPCTelegraphSpec;

/** Describe a projectile's flight. */
typedef struct {
    float speed;
    float width;
} NPCProjectileSpec;

/** Describe a multi-shot pattern.
 *
 * `interval` of zero fires the whole volley at once and `spread_angle` fans it;
 * a positive interval walks the shots across ticks instead. Volley's burst and
 * Ritualist's fan are the same ability shape distinguished by exactly this.
 */
typedef struct {
    int   count;
    float interval;
    float spread_angle;
} NPCBurstSpec;

/** Describe a ground zone left by an ability. */
typedef struct {
    float   radius_tiles;
    float   duration;
    float   tick_rate;
    int     count;                /**< Zones placed per cast; Firebrand lays 5. */
    uint8_t per_tick;             /**< Lay one every tick while moving: Rot Crawler. */
    int     effect_first;         /**< Into the registry effect table. */
    int     effect_count;
} NPCZoneSpec;

/** Describe what a summon brings and how much of it. */
typedef struct {
    int   type_index[NPC_SUMMON_TYPES_MAX];  /**< Resolved type indices. */
    int   type_count;
    int   count_min;
    int   count_max;
    float delay;                  /**< Seconds between cast end and arrival. */
    float spread_tiles;
} NPCSummonSpec;

/** Describe an aura or timed buff applied to allies or self. */
typedef struct {
    float   radius_tiles;         /**< 0 with target_self means self only. */
    uint8_t target_self;
    uint8_t faction_only;         /**< Rally Howl buffs wolves, not everything nearby. */
    uint8_t single_ally;          /**< Tether picks one ally, not the whole pack. */

    /** Force an ally's action rather than modify its stats (V12).
     *
     * Handler's Command and Packmaster's Direct are not buffs: they make an ally
     * act *now*. `ready_allies` clears the ally's cooldowns so its next think
     * fires immediately; `retarget_allies` points it at this NPC's target. Kept
     * as flags on the buff rather than as their own delivery because everything
     * else about them -- radius, faction scoping, who is an ally -- is identical.
     */
    uint8_t ready_allies;
    uint8_t retarget_allies;

    float   duration;
    int     effect_first;
    int     effect_count;
} NPCBuffSpec;

/** Describe movement performed as part of an ability. */
typedef struct {
    float   distance_tiles;
    /** 0 dash, 1 teleport, 2 hop, 3 swap with the nearest ally.
     *
     * Mode 3 is Ashen Acolyte's Decoy Swap, and it is a mode rather than an
     * ability of its own because everything else about it is a teleport -- only
     * the destination differs, and the ally query that answers it already exists.
     */
    uint8_t mode;
    uint8_t damage_on_contact;
    uint8_t terrain_stagger;      /**< Wagon Breaker's Wall Slam. */
    /** Move behind the target rather than toward it: Wraith's Teleport Flank. */
    uint8_t behind_target;
} NPCDisplaceSpec;

/** Describe healing an ability performs. */
typedef struct {
    int     amount;
    int     percent;              /**< Tenths of a percent of max health. */
    float   radius_tiles;
    uint8_t lowest_ally_only;     /**< Rogue Deer picks the worst-off ally. */
} NPCHealSpec;

/* --- Row definitions ----------------------------------------------------- */

/** Identify a faction and the hue every one of its enemies is drawn in. */
typedef struct {
    char  key[NPC_KEY_MAX];
    char  name[NPC_NAME_MAX];
    int   id;
    float color[3];
} NPCFactionDef;

/** Carry every number a movement shape implies, so a type row carries none of them. */
typedef struct {
    char    key[NPC_KEY_MAX];
    uint8_t movement;                 /**< NPCArchMovement. */
    float   move_speed;
    float   aggro_tiles;
    float   leash_tiles;
    float   preferred_tiles;
    float   retreat_tiles;
    float   ally_affinity_tiles;
    uint8_t flee_while_adds_alive;
    uint8_t movement_cc_immune;
    uint8_t never_leashes;
    uint8_t target_priority;          /**< NPCTargetPriority. */
    float   reactive_hop_tiles;       /**< 0 disables the skirmisher hop. */
    float   reactive_hop_cooldown;
    float   reactive_hop_threat_tiles;
    float   movement_noise;           /**< Failed Experiment's erratic approach. */

    /** Server-side collision extent. Deliberately *not* a presentation size:
     * how large the box is drawn is picked by the type's role (§5.1), in one
     * place, by the client-table generator. Two sources for one number is how
     * the drawn size and the hit size drift apart. */
    float   hitbox_radius;
} NPCArchetypeDef;

/** Define one ability: one shape, one telegraph a player learns to recognise. */
typedef struct {
    char    key[NPC_KEY_MAX];
    char    name[NPC_NAME_MAX];
    uint8_t delivery;                 /**< NPCDeliveryKind. */

    float   cast_time;
    float   cooldown;
    float   range_tiles;
    float   recovery;
    int     damage;
    uint8_t damage_type;              /**< AbilityDamageType. */

    /** Treat a component as absent unless its has_ flag is set. */
    uint8_t has_telegraph, has_projectile, has_burst;
    uint8_t has_zone_impact, has_zone_self;
    uint8_t has_summon, has_buff, has_displace, has_heal;

    NPCTelegraphSpec  telegraph;
    NPCProjectileSpec projectile;
    NPCBurstSpec      burst;
    NPCZoneSpec       zone_impact;    /**< Left where the projectile lands. */
    NPCZoneSpec       zone_self;      /**< Centred on the caster. */
    NPCSummonSpec     summon;
    NPCBuffSpec       buff;
    NPCDisplaceSpec   displace;
    NPCHealSpec       heal;

    int self_cost_health_percent;     /**< Blood Ritualist pays HP to cast. */

    /** Effects applied on hit, as a span of the registry's shared effect table. */
    int effect_first;
    int effect_count;
} NPCAbilityDefn;

/** Fire an action on an event rather than on a cooldown.
 *
 * One table replaces threshold phases, on-death spawns, counter windows, panic
 * flees and proximity ambushes -- fourteen behaviours in enemy_types.txt that
 * differ only in what fires them.
 */
typedef struct {
    uint8_t when;                     /**< NPCTriggerWhen. */
    uint8_t action;                   /**< NPCTriggerAction. */
    float   value;                    /**< Percent, seconds or tiles, per `when`. */

    int ability_index;                /**< For CAST and SUMMON; -1 when unused. */
    int from_index, to_index;         /**< For SWAP_ABILITY; -1 when unused. */
    int phase_index;                  /**< For SET_PHASE; -1 when unused. */

    uint8_t once;                     /**< Latch: fire once and stay fired. */
    uint8_t repeating;

    /** Percentage modifiers applied while the trigger holds. */
    float move_speed_pct;
    float attack_speed_pct;
    float damage_pct;
    float damage_taken_pct;
    float cooldown_pct;
    float phase_duration_pct;
} NPCTriggerDef;

/** Restrict a type to a subset of its abilities until an exit condition passes. */
typedef struct {
    char    key[NPC_KEY_MAX];
    int     slot_first;               /**< Into the registry's phase-slot pool. */
    int     slot_count;               /**< Ability slots (type-local) this phase allows. */
    uint8_t exit_kind;                /**< NPCPhaseExit. */
    float   exit_value;
} NPCPhaseDef;

/** Gate damage on a shield, a facing, or a position. */
typedef struct {
    uint8_t has_shield;
    float   shield_damage_taken_pct;  /**< Negative reduces: -60 means 40% taken. */
    uint8_t break_mode;               /**< NPCShieldBreak. */
    int     break_hits;
    float   break_window;
    int     break_pool_hp;
    float   broken_damage_taken_pct;
    uint8_t recharge_never;
    float   down_seconds;

    uint8_t has_frontal_block;
    float   frontal_arc_degrees;
    float   blocked_damage_pct;

    uint8_t has_positional;
    float   positional_radius_tiles;
    float   positional_damage_taken_pct;
} NPCMitigationDef;

/** Compose a modifier onto any base type it is allowed to apply to. */
typedef struct {
    char key[NPC_KEY_MAX];
    char name[NPC_NAME_MAX];
    char name_prefix[NPC_NAME_MAX];
    char name_suffix[NPC_NAME_MAX];

    /** Guards. A zero mask means "no restriction on this axis". */
    uint32_t role_mask;
    uint32_t faction_mask;

    float cast_time_pct;
    float damage_pct;
    float move_speed_pct;
    float attack_speed_pct;
    float damage_taken_pct;

    int trigger_first, trigger_count;

    uint8_t has_outline;
    float   outline[3];
} NPCAffixDef;

/** Compose one enemy from an archetype, a faction, and a list of ability references. */
typedef struct {
    char     key[NPC_KEY_MAX];
    char     name[NPC_NAME_MAX];
    uint16_t id;

    int faction_index;
    int archetype_index;
    uint8_t role;                     /**< NPCRole. */

    int   health;
    int   armor;
    int   xp_reward;
    float hitbox_radius;              /**< 0 means take the archetype's. */
    float move_speed;                 /**< 0 means take the archetype's. */

    /** Overrides of two archetype fields that one type each needs to differ on.
     *
     * Purge Rusher hunts the Blessed and Purifier hunts clusters, but both share
     * their movement with five other enemies that hunt neither -- and Failed
     * Experiment's erratic approach is a property of that creature rather than of
     * `melee_lunger`. Forking an archetype for each would duplicate every number
     * in it, which is exactly what the archetype exists to prevent. */
    int   target_priority;            /**< -1 means take the archetype's. */
    float movement_noise;             /**< 0 means take the archetype's. */

    /** Abilities resolved from keys, with this type's scalar overrides applied. */
    int ability_first;                /**< Into the registry's resolved-ability pool. */
    int ability_count;

    int trigger_first, trigger_count;
    int phase_first, phase_count;
    int affix_first, affix_count;

    uint8_t          has_mitigation;
    NPCMitigationDef mitigation;

    uint8_t summon_only;              /**< No XP, no loot, never in a spawn table. */

    uint8_t stealth;
    uint8_t stealth_damageable;
    float   stealth_revealed_seconds;

    char loot_table[NPC_KEY_MAX];     /**< "none" states the absence explicitly. */
} NPCTypeDef;

/* --- Loading ------------------------------------------------------------- */

/** Load every content file from a directory and resolve all cross-references.
 *
 * Reads factions.json, archetypes.json, abilities.json, affixes.json and types.json
 * from `data_dir`. Every table is sized to what the files contain.
 *
 * @param data_dir  Directory holding the five files, with no trailing slash.
 * @return          1 when every file loaded and every reference resolved, else 0.
 *                  On failure the registry is left empty, never half-loaded.
 */
int npc_registry_load(const char* data_dir);

/** Release every table. Safe to call without a successful load. */
void npc_registry_cleanup(void);

/* --- Lookup -------------------------------------------------------------- */

const NPCTypeDef*      npc_type_get(uint16_t npc_type_id);
const NPCTypeDef*      npc_type_get_by_key(const char* key);
const NPCTypeDef*      npc_type_at(int index);
int                    npc_type_count(void);

const NPCArchetypeDef* npc_archetype_at(int index);
int                    npc_archetype_count(void);

const NPCFactionDef*   npc_faction_at(int index);
int                    npc_faction_count(void);

const NPCAbilityDefn*  npc_ability_at(int index);
const NPCAbilityDefn*  npc_ability_by_key(const char* key);
int                    npc_ability_count(void);

const NPCAffixDef*     npc_affix_at(int index);
int                    npc_affix_count(void);

/** Read one of a type's abilities, overrides already applied.
 *
 * @param slot  0 .. type->ability_count-1.
 * @return      The resolved ability, or NULL when the slot is out of range.
 */
const NPCAbilityDefn* npc_type_ability(const NPCTypeDef* type, int slot);

/** Read one of a type's composed affixes.
 *
 * @param n  0 .. type->affix_count-1.
 * @return   The affix, or NULL when the index is out of range.
 */
const NPCAffixDef* npc_type_affix(const NPCTypeDef* type, int n);

/** Report the registry index of an affix, for storing on a spawned NPC. */
int npc_affix_index_by_key(const char* key);

const NPCTriggerDef*    npc_registry_trigger(int index);
const NPCPhaseDef*      npc_registry_phase(int index);
const AbilityEffectDef* npc_registry_effect(int index);

/** Read a type-local ability slot named by a phase.
 *
 * @param index  phase->slot_first + n.
 * @return       The slot, or -1 when out of range.
 */
int npc_registry_phase_slot(int index);

/* --- Sizing -------------------------------------------------------------- */

/** Report the widest ability_count across every loaded type.
 *
 * npc_world.h sizes its per-NPC cooldown array from this rather than from a
 * compiled constant, which is what removed the MAX_NPC_ABILITIES pair and the
 * out-of-bounds write that keeping the two in step by hand invited.
 */
int npc_registry_max_abilities(void);

/** Report the widest trigger_count across every loaded type, for latch sizing. */
int npc_registry_max_triggers(void);

/** Report the widest phase_count, for per-NPC phase state. */
int npc_registry_max_phases(void);

/** Report the most deferred actions one NPC can queue in a tick.
 *
 * A burst plus a trigger is more than one action, so the deferred queue is sized
 * capacity x this rather than capacity x 1.
 */
int npc_registry_max_actions_per_npc(void);

/* --- Validation ---------------------------------------------------------- */

/** Bound the failures one validation pass reports before it stops listing them. */
#define NPC_VALIDATE_REPORT_MAX 64

/** Check every rule in the design's validation table over the loaded registry.
 *
 * Separate from loading so it can run headless in CI against the shipped files.
 * Reports each failure by file, type key and field.
 *
 * @return The number of failures; zero means the content is servable.
 */
int npc_content_validate(void);

/** Check the loaded content fits the capacities a world is configured for.
 *
 * Separate from npc_content_validate() because it needs the .conf, which CI does
 * not have. Startup calls both; the content test calls only the first.
 *
 * @param max_npcs          Configured pool capacity, or 0 to skip the check.
 * @param npc_effect_slots  Configured per-NPC effect slots, or 0 to skip.
 * @return                  The number of problems; zero means the world can serve it.
 */
int npc_content_check_capacities(int max_npcs, int npc_effect_slots);

#endif // NPC_REGISTRY_H
