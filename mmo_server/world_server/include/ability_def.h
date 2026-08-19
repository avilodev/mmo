/** @file Define JSON-backed ability records and their lookup API. */

#ifndef ABILITY_DEF_H
#define ABILITY_DEF_H

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

/** Identify status effects applied by abilities. */
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
    EFFECT_CLEANSE         = 9      // Remove debuffs
} StatusEffectType;

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

/** Identify character attributes referenced by ability scaling and buffs. */
typedef enum {
    STAT_NONE              = 0,
    STAT_DAMAGE            = 1,     // Legacy — flat damage bonus
    STAT_DEFENSE           = 2,     // Flat defense bonus
    STAT_SPEED             = 3,     // Flat move_speed bonus
    STAT_STRENGTH          = 4,     // Gladiator primary — melee damage
    STAT_AGILITY           = 5,     // Ninja primary — move speed + damage
    STAT_INTELLIGENCE      = 6,     // Landweaver primary — CDR + damage
    STAT_WISDOM            = 7,     // Spirit — mana regen
    STAT_REG               = 8      // Spirit — healing rate
} StatType;

/** Define the dimensions of an ability's optional area. */
typedef struct {
    AbilityAoeShape shape;
    float           radius;         /**< Circle or cone radius in world units. */
    float           angle;          /**< Cone angle in degrees. */
    float           width;          /**< Rectangle width in world units. */
    float           height;         /**< Rectangle height in world units. */
} AbilityAoeDef;

/** Define one timed or immediate status effect. */
typedef struct {
    StatusEffectType type;
    float           duration;       /**< Duration in seconds. */
    float           tick_rate;      /**< Seconds between damage or healing ticks. */
    int             value;          // Damage/heal per tick, slow %, stat bonus, etc.
    StatType        stat;           // Which stat to buff (if type == EFFECT_BUFF)
    uint8_t         reapply;        /**< Reapply the effect when a target re-enters its zone. */
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
    uint8_t             class_id;       // Which class owns this (1-4)
    uint8_t             unlock_level;   // Level required to use

    int                 mana_cost;      // 0 = no mana cost (e.g. gladiator abilities)

    float               cooldown;       // Seconds
    float               cast_time;      // Seconds (0 = instant)

    float               range;          // Max distance
    AbilityTargetType   target_type;    // Who can be targeted

    int                 damage;         // Base damage (0 if healing/utility)
    AbilityDamageType   damage_type;
    StatType            damage_stat;    // Which caster stat scales damage (STAT_NONE = no scaling)
    int                 healing;        // Base healing (0 if damage/utility)

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

// fill at most max_out identifiers and return the number written
int ability_get_class_abilities(uint8_t class_id, uint16_t* out_ids, int max_out);

int abilities_get_count(void);

void abilities_cleanup(void);

#endif // ABILITY_DEF_H