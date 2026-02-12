// ============================================================================
// npc_ai.h — NPC AI system: behavior profiles, state machine, ability usage
//
// Data-driven NPC behaviors loaded from npc_types.json. Each NPC type gets
// a movement pattern, aggro range, and a set of abilities:
//   - Projectile: fires a traveling projectile (via projectile_spawn)
//   - Telegraph:  FF14-style ground indicator with cast time, then AOE damage
//
// Telegraph flow:
//   1. NPC starts cast -> NPC_TELEGRAPH_START sent to nearby players
//   2. Client shows ground indicator (shape, position, duration)
//   3. NPC is locked (no movement/other abilities) during cast
//   4. Cast completes -> damage all players still in the shape
//   5. NPC_TELEGRAPH_RESOLVE sent, ability goes on cooldown
// ============================================================================

#ifndef NPC_AI_H
#define NPC_AI_H

#include "combat_config.h"
#include <stdint.h>

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

#define MAX_NPC_ABILITIES     4       // Max abilities per NPC type
#define MAX_NPC_AI_PROFILES   32      // Max distinct NPC type profiles

// ---------------------------------------------------------------------------
// NPC AI state machine
// ---------------------------------------------------------------------------

typedef enum {
    NPC_AI_IDLE       = 0,   // Standing at spawn, no target
    NPC_AI_AGGRO      = 1,   // Has a target, chasing/attacking
    NPC_AI_RETURNING  = 2,   // Lost target or leashed, walking back to spawn
    NPC_AI_CASTING    = 3    // Casting a telegraph ability (locked in place)
} NPCAIState;

// ---------------------------------------------------------------------------
// NPC movement behavior
// ---------------------------------------------------------------------------

typedef enum {
    NPC_MOVE_STATIONARY      = 0,   // Never moves, shoots from spawn
    NPC_MOVE_FOLLOW          = 1,   // Chases target into melee range
    NPC_MOVE_MAINTAIN_RANGE  = 2    // Keeps preferred_range distance from target
} NPCMovementType;

// ---------------------------------------------------------------------------
// Ability delivery type
// ---------------------------------------------------------------------------

typedef enum {
    NPC_DELIVERY_PROJECTILE  = 0,   // Fires a traveling projectile
    NPC_DELIVERY_TELEGRAPH   = 1    // Ground indicator -> AOE damage after cast time
} NPCDeliveryType;

// ---------------------------------------------------------------------------
// Telegraph shape (for ground indicators)
// ---------------------------------------------------------------------------

typedef enum {
    NPC_TELEGRAPH_CIRCLE     = 0,
    NPC_TELEGRAPH_CONE       = 1,
    NPC_TELEGRAPH_RECTANGLE  = 2,
    NPC_TELEGRAPH_LINE       = 3
} NPCTelegraphShape;

// ---------------------------------------------------------------------------
// NPC ability definition
// ---------------------------------------------------------------------------

typedef struct {
    uint16_t ability_id;         // For client VFX lookup
    int      damage;             // Base damage
    float    range;              // Max range to use this ability
    float    cooldown;           // Seconds between uses

    // Delivery type
    uint8_t  delivery;           // NPCDeliveryType

    // Projectile fields (delivery == NPC_DELIVERY_PROJECTILE)
    float    projectile_speed;   // >0 spawns projectile
    float    projectile_width;   // Hitbox width

    // Telegraph fields (delivery == NPC_DELIVERY_TELEGRAPH)
    float    cast_time;          // Seconds of telegraph before damage resolves
    uint8_t  telegraph_shape;    // NPCTelegraphShape
    float    telegraph_radius;   // Circle/cone radius
    float    telegraph_angle;    // Cone angle in degrees
    float    telegraph_width;    // Rectangle/line width
    float    telegraph_length;   // Rectangle/line length
    uint8_t  telegraph_at_target; // 0 = centered on NPC, 1 = centered on target position
} NPCAbilityDef;

// ---------------------------------------------------------------------------
// NPC AI profile — one per npc_type_id, loaded from JSON
// ---------------------------------------------------------------------------

typedef struct {
    uint16_t        npc_type_id;

    // Movement
    NPCMovementType movement_type;
    float           move_speed;         // World units per second
    float           preferred_range;    // For MAINTAIN_RANGE: desired distance to target

    // Aggro
    float           aggro_range;        // Detection distance
    float           leash_range;        // Max distance from spawn before giving up

    // Abilities
    NPCAbilityDef   abilities[MAX_NPC_ABILITIES];
    int             ability_count;
} NPCAIProfile;

// ---------------------------------------------------------------------------
// API
// ---------------------------------------------------------------------------

// Load NPC AI profiles from JSON. Call once at startup.
int npc_ai_init(const char* json_path);

// Cleanup.
void npc_ai_cleanup(void);

// Per-tick AI update. Call from combat_update_thread at 20Hz.
// Handles: target acquisition, movement, ability usage, casting, telegraph resolve.
void npc_ai_tick(NPCWorld* world, double delta_time);

// Get the AI profile for a given npc_type_id. Returns NULL if none.
const NPCAIProfile* npc_ai_get_profile(uint16_t npc_type_id);

#endif // NPC_AI_H
