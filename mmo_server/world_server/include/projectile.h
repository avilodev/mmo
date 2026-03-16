// ============================================================================
// projectile.h — Modular projectile system
//
// Manages all projectiles in the world: player skillshots, NPC ranged attacks.
// Ticked from combat_update_thread, broadcast from projectile_broadcast_thread.
// ============================================================================

#ifndef PROJECTILE_H
#define PROJECTILE_H

#include "combat_config.h"
#include "ability_def.h"
#include "types.h"

#include <stdint.h>
#include <pthread.h>

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

#define MAX_PROJECTILES           128
#define PROJECTILE_VIEW_RANGE    2000.0f

// ---------------------------------------------------------------------------
// Enums
// ---------------------------------------------------------------------------

typedef enum {
    PROJECTILE_OWNER_PLAYER = 0,
    PROJECTILE_OWNER_NPC    = 1
} ProjectileOwnerType;

typedef enum {
    PROJECTILE_DESTROY_EXPIRED  = 0,
    PROJECTILE_DESTROY_HIT      = 1,
    PROJECTILE_DESTROY_CANCELLED = 2
} ProjectileDestroyReason;

// ---------------------------------------------------------------------------
// Projectile entity
// ---------------------------------------------------------------------------

typedef struct {
    uint8_t     is_active;
    uint32_t    projectile_id;

    // Ownership
    uint8_t     owner_type;         // ProjectileOwnerType
    uint32_t    owner_id;           // Character ID or NPC ID
    uint16_t    ability_id;         // Source ability (client uses for VFX)
    int         owner_fd;           // Socket fd of the owner (for hit feedback)

    // Physics
    float       pos_x, pos_y;
    float       dir_x, dir_y;      // Unit direction vector
    float       speed;              // World units per second
    float       width;              // Hitbox width
    float       max_range;
    float       distance_traveled;

    // Damage
    int         damage;
    AbilityDamageType damage_type;
    AbilityBonusDamageDef bonus_damage;

    // Caster stat snapshot (for player projectiles — damage calc at hit time)
    int         caster_strength;
    int         caster_agility;
    int         caster_intelligence;
    int         caster_wisdom;
    int         damage_stat;        // StatType int — which stat scales this projectile's damage

    // Status effects to apply on hit
    AbilityEffectDef effects[MAX_ABILITY_EFFECTS];
    uint8_t     effect_count;
} Projectile;

// ---------------------------------------------------------------------------
// Spawn info — clean input struct for creating projectiles
// ---------------------------------------------------------------------------

typedef struct {
    uint8_t     owner_type;         // PROJECTILE_OWNER_PLAYER or _NPC
    uint32_t    owner_id;
    uint16_t    ability_id;
    int         owner_fd;

    // Origin and aim
    float       origin_x, origin_y;
    float       aim_x, aim_y;

    // Physics
    float       speed;
    float       width;
    float       max_range;

    // Damage
    int         damage;
    AbilityDamageType damage_type;
    AbilityBonusDamageDef bonus_damage;

    // Caster stats (for player projectiles)
    int         caster_strength;
    int         caster_agility;
    int         caster_intelligence;
    int         caster_wisdom;
    int         damage_stat;        // StatType int — which stat scales this projectile's damage

    // Effects on hit
    AbilityEffectDef effects[MAX_ABILITY_EFFECTS];
    uint8_t     effect_count;
} ProjectileSpawnInfo;

// ---------------------------------------------------------------------------
// API
// ---------------------------------------------------------------------------

// Initialize the projectile system. Call once at startup.
void projectile_init(void);

// Cleanup. Call at shutdown.
void projectile_cleanup(void);

// Spawn a new projectile. Returns the assigned projectile_id, or 0 on failure.
uint32_t projectile_spawn(const ProjectileSpawnInfo* info);

// Per-tick update. Moves projectiles, checks collisions, deals damage.
// Call from combat_update_thread at 20Hz.
void projectile_tick(NPCWorld* world, double delta_time);

// Broadcast projectile positions to nearby players.
// Call from projectile_broadcast_thread at 30Hz.
// Takes a snapshot of active players (pos + fd) to avoid holding locks during I/O.
void projectile_broadcast(void);

// Remove a specific projectile by ID.
void projectile_remove(uint32_t projectile_id);

#endif // PROJECTILE_H
