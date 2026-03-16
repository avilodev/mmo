// ============================================================================
// ability_def.h — Ability definitions loaded from JSON at startup
//
// Same pattern as items_database.h — static lookup table, parsed once.
// ============================================================================

#ifndef ABILITY_DEF_H
#define ABILITY_DEF_H

#include <stdint.h>

#define MAX_ABILITIES       128
#define MAX_ABILITY_NAME    32
#define MAX_ABILITY_EFFECTS 4       // Max status effects per ability
#define MAX_ABILITY_KEY     32      // JSON key / string ID (e.g. "cleave")

// ============================================================================
// Enums
// ============================================================================

typedef enum {
    ABILITY_TARGET_ENEMY   = 0,     // Targets enemies (default)
    ABILITY_TARGET_ALLY    = 1,     // Targets allies
    ABILITY_TARGET_SELF    = 2,     // Self-cast only
    ABILITY_TARGET_GROUND  = 3      // Targets a position (AOE placement)
} AbilityTargetType;

typedef enum {
    ABILITY_AOE_NONE       = 0,
    ABILITY_AOE_CIRCLE     = 1,
    ABILITY_AOE_CONE       = 2,
    ABILITY_AOE_RECTANGLE  = 3
} AbilityAoeShape;

typedef enum {
    ABILITY_DMG_PHYSICAL   = 0,
    ABILITY_DMG_EARTH      = 1,
    ABILITY_DMG_SPIRIT     = 2
} AbilityDamageType;

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

typedef enum {
    SPAWN_NONE             = 0,
    SPAWN_WALL             = 1,     // Collision obstacle
    SPAWN_ZONE             = 2,     // AOE zone (no collision)
    SPAWN_DECOY            = 3      // Decoy/clone entity
} SpawnEntityType;

typedef enum {
    MOVEMENT_NONE          = 0,
    MOVEMENT_TELEPORT      = 1,     // Instant reposition
    MOVEMENT_DASH          = 2      // Fast move (can be interrupted)
} AbilityMovementType;

typedef enum {
    PROJECTILE_NONE        = 0,
    PROJECTILE_LINEAR      = 1      // Straight line skillshot
} AbilityProjectileType;

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

// ============================================================================
// Sub-structures (optional components of an ability)
// ============================================================================

typedef struct {
    AbilityAoeShape shape;
    float           radius;         // For circle/cone
    float           angle;          // For cone (degrees)
    float           width;          // For rectangle
    float           height;         // For rectangle
} AbilityAoeDef;

typedef struct {
    StatusEffectType type;
    float           duration;       // Seconds
    float           tick_rate;      // Seconds between ticks (DOT/HOT)
    int             value;          // Damage/heal per tick, slow %, stat bonus, etc.
    StatType        stat;           // Which stat to buff (if type == EFFECT_BUFF)
    uint8_t         reapply;        // 1 = reapply on zone re-entry
} AbilityEffectDef;

typedef struct {
    SpawnEntityType type;
    float           duration;       // How long it lasts
    uint8_t         has_collision;  // 1 = blocks movement
    int             hp;             // Destructible? 0 = invulnerable
    uint8_t         inherits_appearance; // For decoys
} AbilitySpawnDef;

typedef struct {
    AbilityMovementType type;
    float               distance;
    // direction is always toward aim point
} AbilityMovementDef;

typedef struct {
    AbilityProjectileType type;
    float                 speed;    // World units per second
    float                 width;    // Hitbox width
} AbilityProjectileDef;

typedef struct {
    uint8_t             condition;  // 0=none, 1=target_below_hp_percent
    float               threshold;  // e.g. 30.0 for 30%
    float               multiplier; // e.g. 2.5x damage
} AbilityBonusDamageDef;

// ============================================================================
// Main ability definition
// ============================================================================

typedef struct {
    // Identity
    uint16_t            id;             // Numeric ID (assigned at load time, 1-based)
    char                key[MAX_ABILITY_KEY];   // String key from JSON (e.g. "cleave")
    char                name[MAX_ABILITY_NAME]; // Display name
    uint8_t             class_id;       // Which class owns this (1-4)
    uint8_t             unlock_level;   // Level required to use

    // Costs
    int                 mana_cost;      // 0 = no mana cost (e.g. gladiator abilities)

    // Timing
    float               cooldown;       // Seconds
    float               cast_time;      // Seconds (0 = instant)

    // Targeting
    float               range;          // Max distance
    AbilityTargetType   target_type;    // Who can be targeted

    // Damage / Healing
    int                 damage;         // Base damage (0 if healing/utility)
    AbilityDamageType   damage_type;
    StatType            damage_stat;    // Which caster stat scales damage (STAT_NONE = no scaling)
    int                 healing;        // Base healing (0 if damage/utility)

    // Optional components — check .type != NONE to see if present
    AbilityAoeDef       aoe;
    AbilityEffectDef    effects[MAX_ABILITY_EFFECTS];
    uint8_t             effect_count;
    AbilitySpawnDef     spawn;
    AbilityMovementDef  movement;
    AbilityProjectileDef projectile;
    AbilityBonusDamageDef bonus_damage;

    // Client hints (server stores these for broadcasting to clients)
    char                animation[32];
    char                sfx[32];
    char                vfx[32];
    char                image[32];   // Icon filename, e.g. "cleave.png" — served in AbilityDataPacket
} AbilityDef;

// ============================================================================
// API — matches the items_database pattern
// ============================================================================

// Load all abilities from JSON file. Call once at startup.
int abilities_init(const char* json_filepath);

// Get ability by numeric ID (1-based). Returns NULL if not found.
const AbilityDef* ability_get(uint16_t ability_id);

// Get ability by string key (e.g. "cleave"). Returns NULL if not found.
const AbilityDef* ability_get_by_key(const char* key);

// Get all ability IDs for a given class. Returns count.
// Fills out_ids[] with up to max_out ability IDs.
int ability_get_class_abilities(uint8_t class_id, uint16_t* out_ids, int max_out);

// Get total loaded count
int abilities_get_count(void);

// Cleanup
void abilities_cleanup(void);

#endif // ABILITY_DEF_H