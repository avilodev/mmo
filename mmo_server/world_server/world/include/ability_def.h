/** @file Define JSON-backed ability records and their lookup API. */

#ifndef ABILITY_DEF_H
#define ABILITY_DEF_H

#include "protocol.h"

#include <stdint.h>

/** Bound loaded abilities and their fixed-width text and effect fields. */
#define MAX_ABILITIES       128
#define MAX_ABILITY_NAME    32
#define MAX_ABILITY_EFFECTS 4       /**< Maximum status effects attached to one ability. */
#define MAX_ABILITY_KEY     32      /**< Capacity of a JSON string identifier. */

/** Identify the entity or position an ability may target. */
typedef enum {
    ABILITY_TARGET_ENEMY   = 0,     // Targets enemies (default)
    ABILITY_TARGET_ALLY    = 1,     // Targets allies
    ABILITY_TARGET_SELF    = 2,     // Self-cast only
    ABILITY_TARGET_GROUND  = 3      // Targets a position (AOE placement)
} AbilityTargetType;

/** Identify optional area geometry for an ability. */
typedef enum {
    ABILITY_AOE_NONE       = 0,
    ABILITY_AOE_CIRCLE     = 1,
    ABILITY_AOE_CONE       = 2,
    ABILITY_AOE_RECTANGLE  = 3
} AbilityAoeShape;

/** Identify the damage school used by an ability. */
typedef enum {
    ABILITY_DMG_PHYSICAL   = 0,
    ABILITY_DMG_EARTH      = 1,
    ABILITY_DMG_SPIRIT     = 2
} AbilityDamageType;

/** Identify status effects applied by abilities.
 *
 * Everything from EFFECT_TAUNT down is a primitive the Blessed kits introduced. They
 * are the only engine work the three designed kits actually needed: once a verb
 * exists here, every further ability using it is a JSON entry and an icon.
 */
typedef enum {
    EFFECT_NONE            = 0,
    EFFECT_DOT             = 1,     // Damage over time
    EFFECT_HOT             = 2,     // Heal over time
    EFFECT_STUN            = 3,
    EFFECT_SLOW            = 4,
    EFFECT_BUFF            = 5,     // Stat buff (uses stat + value)
    EFFECT_STEALTH         = 6,
    EFFECT_KNOCKUP         = 7,
    EFFECT_LINK            = 8,     // Spirit link (share damage)
    EFFECT_CLEANSE         = 9,     // Remove debuffs

    EFFECT_TAUNT           = 10,    // Force hostile NPCs to target the caster
    EFFECT_RESOURCE        = 11,    // Restore a percentage of max resource
    EFFECT_DAMAGE_TAKEN    = 12,    // Percentage damage-taken modifier
    EFFECT_DAMAGE_DEALT    = 13,    // Percentage damage-dealt modifier
    EFFECT_ROOT            = 14,    // Movement locked; actions still allowed
    EFFECT_CHANNEL         = 15,    // Actions locked for the duration
    EFFECT_HOT_PERCENT     = 16,    // Heal over time as a percentage of max health per tick

    /* The five NPC kits introduce. Each is a predicate over an existing gate
     * rather than a new subsystem: blind and fear read in movement and aim,
     * charm redirects a target, form-lock rejects a swap, mark is a damage-taken
     * modifier that also tells the client to draw something over the target. */
    EFFECT_BLIND           = 17,    // Aim is scattered; `value` is the cone half-angle in degrees
    EFFECT_FEAR            = 18,    // Movement inverted away from the source; actions still allowed
    EFFECT_CHARM           = 19,    // Target selection forced toward `source_id`
    EFFECT_FORM_LOCK       = 20,    // Form swap refused for the duration
    EFFECT_MARK            = 21,    // Damage taken raised by `value` tenths of a percent

    EFFECT_COUNT
} StatusEffectType;

/** Report whether an effect's `value` is expressed in tenths of a percent.
 *
 * Percentages travel as integers so an ability stays one JSON object of plain
 * numbers: 100 means 10.0%. This predicate is the single place that knows which
 * effects read their value that way.
 */
static inline int effect_value_is_permille(StatusEffectType type) {
    return type == EFFECT_RESOURCE     || type == EFFECT_DAMAGE_TAKEN ||
           type == EFFECT_DAMAGE_DEALT || type == EFFECT_HOT_PERCENT  ||
           type == EFFECT_MARK;
}

/** Convert an effect value in tenths of a percent to a fraction: 100 becomes 0.10. */
static inline double effect_permille_to_fraction(int value) {
    return (double)value / 1000.0;
}

/** Identify optional entities spawned by an ability. */
typedef enum {
    SPAWN_NONE             = 0,
    SPAWN_WALL             = 1,     // Collision obstacle
    SPAWN_ZONE             = 2,     // AOE zone (no collision)
    SPAWN_DECOY            = 3      // Decoy/clone entity
} SpawnEntityType;

/** Identify movement performed as part of an ability. */
typedef enum {
    MOVEMENT_NONE          = 0,
    MOVEMENT_TELEPORT      = 1,     // Instant reposition
    MOVEMENT_DASH          = 2      // Fast move (can be interrupted)
} AbilityMovementType;

/** Identify projectile behavior attached to an ability. */
typedef enum {
    PROJECTILE_NONE        = 0,
    PROJECTILE_LINEAR      = 1      // Straight line skillshot
} AbilityProjectileType;

/** Identify what an ability scales with or buffs.
 *
 * Values from 0 to STAT_COUNT-1 are StatId indices, so an ability naming "strength"
 * or "ferocity" means the same attribute the race registry and the wire do. The two
 * entries above STAT_COUNT are derived quantities that are not attributes but can
 * still be buffed; adding another is one enum entry and one JSON key.
 */
typedef enum {
    /** 0 .. STAT_COUNT-1 are StatId values and need no entries of their own. */
    STAT_TARGET_MOVE_SPEED   = STAT_COUNT,      // Flat move_speed bonus
    STAT_TARGET_WEAPON_DAMAGE,                  // Flat weapon damage bonus
    STAT_TARGET_COUNT,
    STAT_TARGET_NONE         = -1               // No scaling and no buff target
} StatType;

/** Report whether a buff target names one of the eleven character attributes. */
static inline int stat_target_is_attribute(StatType target) {
    return (int)target >= 0 && (int)target < (int)STAT_COUNT;
}

/** Define the dimensions of an ability's optional area. */
typedef struct {
    AbilityAoeShape shape;
    float           radius;         /**< Circle or cone radius in world units. */
    float           angle;          /**< Cone angle in degrees. */
    float           width;          /**< Rectangle width in world units. */
    float           height;         /**< Rectangle height in world units. */
} AbilityAoeDef;

/** Define one timed or immediate status effect.
 *
 * `value` carries whatever the effect's type means by it: damage or healing per
 * tick, a slow percentage, a stat bonus, or — for the effects
 * effect_value_is_permille() names — tenths of a percent.
 */
typedef struct {
    StatusEffectType type;
    float           duration;       /**< Duration in seconds. */
    float           tick_rate;      /**< Seconds between damage or healing ticks. */
    int             value;          // Damage/heal per tick, slow %, stat bonus, etc.
    StatType        stat;           // Which stat to buff (if type == EFFECT_BUFF)
    uint8_t         reapply;        /**< Reapply the effect when a target re-enters its zone. */
    uint8_t         self;           /**< Apply to the caster rather than to the target. */
    /** Apply only when the ability's bonus-damage condition held.
     *
     * This is what makes Rend one ability rather than two: its self damage and
     * resistance buff fire on the execute branch and nowhere else. */
    uint8_t         on_condition;
} AbilityEffectDef;

/** Define an entity spawned for an ability's duration. */
typedef struct {
    SpawnEntityType type;
    float           duration;       /**< Lifetime in seconds. */
    uint8_t         has_collision;  /**< Nonzero when the entity blocks movement. */
    int             hp;             /**< Destructible health, or zero for invulnerability. */
    uint8_t         inherits_appearance; /**< Copy the caster's appearance for decoys. */
} AbilitySpawnDef;

/** Define movement toward the cast's aim point. */
typedef struct {
    AbilityMovementType type;
    float               distance;
} AbilityMovementDef;

/** Define a projectile's speed and collision width. */
typedef struct {
    AbilityProjectileType type;
    float                 speed;    /**< World units traveled per second. */
    float                 width;    /**< Collision width in world units. */
} AbilityProjectileDef;

/** Define conditional damage scaling below a health threshold. */
typedef struct {
    uint8_t             condition;  // 0=none, 1=target_below_hp_percent
    float               threshold;  /**< Health threshold as a percentage. */
    float               multiplier; /**< Damage multiplier applied when the condition holds. */
} AbilityBonusDamageDef;

/** Aggregate identity, cost, targeting, effects, and client hints for one ability. */
typedef struct {
    uint16_t            id;             // Numeric ID (assigned at load time, 1-based)
    char                key[MAX_ABILITY_KEY];   // String key from JSON (e.g. "cleave")
    char                name[MAX_ABILITY_NAME]; // Display name
    /** Which race owns this ability, or 0 for a universal Human Form ability.
     *
     * Human Form's kit is identical for every race, so it is the one case where an
     * ability belongs to no race at all. */
    uint8_t             race_id;
    uint8_t             form;           // PlayerForm this ability is usable in
    uint8_t             unlock_level;   // Level required to use

    /** Cost in whichever pool the caster's role selects. Always 0 in Human Form,
     * which is cooldown-only and has no pool. */
    int                 resource_cost;

    float               cooldown;       // Seconds
    float               cast_time;      // Seconds (0 = instant)

    float               range;          // Max distance
    AbilityTargetType   target_type;    // Who can be targeted

    int                 damage;         // Base damage (0 if healing/utility)
    AbilityDamageType   damage_type;
    StatType            damage_stat;    // Which caster stat scales damage (STAT_TARGET_NONE = none)
    int                 healing;        // Base healing (0 if damage/utility)
    /** Immediate healing in tenths of a percent of max health: 50 means 5.0%.
     *
     * Percent-of-max healing is what Bite, Hibernate and Second Wind all express and
     * the one shape a flat integer cannot. */
    int                 heal_percent;
    /** Direct the healing at the caster rather than the target, as Bite's self-heal does. */
    uint8_t             heal_self;

    /** Treat optional components with a NONE type as absent. */
    AbilityAoeDef       aoe;
    AbilityEffectDef    effects[MAX_ABILITY_EFFECTS];
    uint8_t             effect_count;
    AbilitySpawnDef     spawn;
    AbilityMovementDef  movement;
    AbilityProjectileDef projectile;
    AbilityBonusDamageDef bonus_damage;

    char                animation[32];
    char                sfx[32];
    char                vfx[32];
    char                image[32];   // Icon filename, e.g. "cleave.png" — served in AbilityDataPacket
} AbilityDef;

// load once during world-server startup
int abilities_init(const char* json_filepath);

// return a registry-owned definition or NULL for an unknown identifier
const AbilityDef* ability_get(uint16_t ability_id);

// return a registry-owned definition or NULL for an unknown key
const AbilityDef* ability_get_by_key(const char* key);

/** Fill the hotbar for one race and form.
 *
 * Human Form's five abilities are universal, so FORM_HUMAN ignores `race_id` and
 * returns the same kit for every race — which is what makes Human Form provably
 * identical across races rather than merely intended to be.
 *
 * @param out_ids  Receives at most max_out ability identifiers.
 * @return         The number written.
 */
int ability_get_form_abilities(uint8_t race_id, uint8_t form, uint16_t* out_ids, int max_out);

/** Resolve a spec's ability keys to identifiers, in the order the spec lists them.
 *
 * @param keys       Ability keys, one per hotbar slot.
 * @param key_count  How many keys are present.
 * @param out_ids    Receives at most max_out identifiers.
 * @return           The number written; unresolved keys are reported and skipped.
 */
int ability_resolve_keys(const char keys[][MAX_ABILITY_KEY], int key_count,
                         uint16_t* out_ids, int max_out);

int abilities_get_count(void);

void abilities_cleanup(void);

#endif // ABILITY_DEF_H