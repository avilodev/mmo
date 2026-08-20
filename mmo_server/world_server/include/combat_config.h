/** @file Define authoritative attack profiles, NPC state, and pending casts. */

#ifndef COMBAT_CONFIG_H
#define COMBAT_CONFIG_H

#include "protocol.h"

#include <stdint.h>
#include <pthread.h>

#define MAX_NPC_ABILITIES_RT 4  /**< Match MAX_NPC_ABILITIES in npc_ai.h. */

/** Define authoritative timing, geometry, and damage for one race's basic attack.
 *
 * One profile per race, loaded from attack_profiles.json. Nothing here knows how many
 * races exist: the array is sized by the MAX_RACES bound and indexed by the same
 * fused identifier everything else uses.
 */
typedef struct {
    float       cast_time;          // Seconds from intent to damage resolution
    float       cooldown;           // Seconds after damage before next attack can start
    float       range;              // Max distance (world units) for target resolution
    int         base_damage;        // Flat damage before variance
    int         damage_variance;    // ±% applied randomly at resolution time
    uint8_t     attack_type;        // AttackType: SINGLE / AOE / CONE / LINE
    float       cone_half_angle;    // Degrees — only used when attack_type == CONE
    float       line_width;         // World units — only used when attack_type == LINE
    /** Configure projectile resolution for ranged class attacks. */
    uint8_t     is_ranged;          // 1 = spawn projectile at cast resolution instead of instant damage
    float       projectile_speed;   // World units per second
    float       projectile_width;   // Hitbox width of the projectile
    uint8_t     projectile_damage_stat;  // StatId that scales projectile damage
    uint8_t     projectile_damage_type;  // AbilityDamageType (0=phys, 1=earth, 2=spirit)
    /** Which attribute scales this attack's damage. */
    uint8_t     damage_stat;             // StatId
    uint8_t     is_loaded;               /**< Nonzero once a profile has been read for this race. */
} RaceAttackProfile;

/** Index attack profiles by race identifier, leaving index zero unused. */
extern RaceAttackProfile g_race_profiles[MAX_RACES + 1];

/** Identify NPC disposition used by AI and client presentation. */
typedef enum {
    NPC_CATEGORY_PASSIVE    = 0,   // Village NPCs, vendors — never attacks
    NPC_CATEGORY_HOSTILE    = 1,   // Enemies — will aggro and attack
    NPC_CATEGORY_QUEST      = 2,   // Quest givers — interactable, special marker
} NPCCategory;

/** Hold authoritative combat, spawn, dialogue, and AI state for one NPC. */
typedef struct {
    uint32_t    id;                // Unique entity ID (server-assigned)
    char        name[32];
    float       pos_x, pos_y;      // Current world position
    int         health;
    int         max_health;
    float       hitbox_radius;     // For collision / hit detection

    /** Flat damage reduction, matching the Armor attribute players carry.
     *
     * There is no evasion counterpart: the Blessed model drops dodge entirely, so an
     * attack that reaches an NPC always connects. */
    int         armor;
    uint32_t    xp_reward;         // XP granted to killer

    /* No coin reward: enemies drop items, which the killer sells to a
     * kingdom's NPCs for that kingdom's currency. See loot.h. */

    uint8_t     is_alive;          // 0 = dead, 1 = alive
    uint8_t     category;          // NPCCategory — passive/hostile/quest

    /** Retain spawn state used for return and respawn behavior. */
    float       spawn_x, spawn_y;  // Original spawn position
    float       respawn_time;      // Seconds until respawn (0 = no respawn)
    double      death_time;        // When the NPC died (CLOCK_MONOTONIC)
    uint16_t    npc_type_id;       // For loot table lookup

    /** Associate optional local dialogue data with an interactable NPC. */
    uint32_t    dialogue_id;       // 0 = no dialogue, otherwise dialogue ID from JSON
    uint8_t     is_interactable;   // 1 if player can talk to this NPC

    /** Track mutable AI target and cooldown state. */
    uint8_t     ai_state;          // NPCAIState: 0=idle, 1=aggro, 2=returning, 3=casting
    uint32_t    ai_target_id;      // Current target character_id (0 = no target)
    /** Force this NPC's target while a taunt holds.
     *
     * Roar is the tank's whole job in one ability, so the taunt has to override
     * target selection rather than merely nudge it. It is an absolute expiry on the
     * monotonic clock, for the same reason ability cooldowns are. */
    uint32_t    taunt_source_id;
    double      taunt_expires_at;
    double      ai_ability_cooldowns[MAX_NPC_ABILITIES_RT]; // Last use time per ability slot
    uint8_t     ai_cd_seeded;      // 1 once per-enemy cooldown phases have been randomized

    /** Preserve geometry while an NPC telegraph cast is active. */
    uint8_t     ai_is_casting;     // 1 = currently casting a telegraph
    int         ai_cast_ability_idx; // Which ability slot is being cast
    double      ai_cast_start;     // When the cast began (CLOCK_MONOTONIC)
    float       ai_cast_pos_x;     // Telegraph center position
    float       ai_cast_pos_y;
    float       ai_cast_dir_x;     // Telegraph direction (for cone/rect/line)
    float       ai_cast_dir_y;
} NPCEntity;

/** Bound the fixed NPC pool owned by one world process. */
#define MAX_NPCS    256

/** Protect a fixed pool of active NPC entities with one mutex. */
typedef struct {
    NPCEntity   npcs[MAX_NPCS];
    int         count;              // How many slots are in use
    pthread_mutex_t lock;           // Protects the entire array
} NPCWorld;

/** Snapshot an in-flight basic attack between intent and resolution. */
typedef struct {
    uint8_t     is_active;          // 1 if this attacker has a pending cast
    /** Identify the character that started this cast.
     *
     * The array is indexed by player slot, and slots are recycled on logout. Without
     * an owner the cast of a player who disconnects mid-cast would resolve as whoever
     * next occupies the slot, handing them the damage credit, XP, loot, and quest kill.
     * Every reader must confirm this matches the character now in the slot.
     */
    uint32_t    character_id;
    double      cast_start_time;    // Epoch seconds when cast began
    float       cast_duration;      // How long it takes (from the race's profile)
    uint8_t     attack_type;        // Which shape to resolve at completion
    float       origin_x, origin_y; // Attacker position AT CAST START (snapshot)
    float       aim_x, aim_y;       // Aim point AT CAST START (snapshot)
    float       range;              // From the race's profile, snapshotted
    int         base_damage;
    int         damage_variance;
    float       cone_half_angle;    // Degrees, from the race's profile
    float       line_width;         // World units, from the race's profile
    float       cooldown;           // Stored so we can set last_attack_time on resolve
    uint8_t     damage_stat;        // StatId the race's profile scales this attack with
    /** Retain projectile parameters when the cast resolves at range. */
    uint8_t     is_ranged;
    float       projectile_speed;
    float       projectile_width;
    uint8_t     projectile_damage_stat;
    uint8_t     projectile_damage_type;
} PendingCast;

#endif // COMBAT_CONFIG_H