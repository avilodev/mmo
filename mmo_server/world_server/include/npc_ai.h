/** @file Define data-driven NPC movement, targeting, abilities, and telegraph state. */

#ifndef NPC_AI_H
#define NPC_AI_H

#include "combat_config.h"
#include "tick_snapshot.h"
#include <stdint.h>

#define MAX_NPC_ABILITIES     4       /**< Maximum abilities for one NPC type. */
#define MAX_NPC_AI_PROFILES   32      /**< Maximum distinct NPC behavior profiles. */

/** Identify the current state of an NPC's behavior machine. */
typedef enum {
    NPC_AI_IDLE       = 0,   // Standing at spawn, no target
    NPC_AI_AGGRO      = 1,   // Has a target, chasing/attacking
    NPC_AI_RETURNING  = 2,   // Lost target or leashed, walking back to spawn
    NPC_AI_CASTING    = 3    // Casting a telegraph ability (locked in place)
} NPCAIState;

/** Identify how an NPC positions itself relative to its target. */
typedef enum {
    NPC_MOVE_STATIONARY      = 0,   // Never moves, shoots from spawn
    NPC_MOVE_FOLLOW          = 1,   // Chases target into melee range
    NPC_MOVE_MAINTAIN_RANGE  = 2    // Keeps preferred_range distance from target
} NPCMovementType;

/** Select projectile or delayed-area delivery for an NPC ability. */
typedef enum {
    NPC_DELIVERY_PROJECTILE  = 0,   // Fires a traveling projectile
    NPC_DELIVERY_TELEGRAPH   = 1    // Ground indicator -> AOE damage after cast time
} NPCDeliveryType;

/** Identify the client-visible geometry of an NPC telegraph. */
typedef enum {
    NPC_TELEGRAPH_CIRCLE     = 0,
    NPC_TELEGRAPH_CONE       = 1,
    NPC_TELEGRAPH_RECTANGLE  = 2,
    NPC_TELEGRAPH_LINE       = 3
} NPCTelegraphShape;

/** Define one NPC attack's range, timing, and delivery geometry. */
typedef struct {
    uint16_t ability_id;         // For client VFX lookup
    int      damage;             // Base damage
    float    range;              // Max range to use this ability
    float    cooldown;           // Seconds between uses

    uint8_t  delivery;           // NPCDeliveryType

    /** Configure projectile delivery when selected. */
    float    projectile_speed;   // >0 spawns projectile
    float    projectile_width;   // Hitbox width

    /** Configure delayed telegraph delivery when selected. */
    float    cast_time;          // Seconds of telegraph before damage resolves
    uint8_t  telegraph_shape;    // NPCTelegraphShape
    float    telegraph_radius;   // Circle/cone radius
    float    telegraph_angle;    // Cone angle in degrees
    float    telegraph_width;    // Rectangle/line width
    float    telegraph_length;   // Rectangle/line length
    uint8_t  telegraph_at_target;  // 0 = centered on NPC, 1 = centered on target position
    uint8_t  teleport_on_resolve;  // 1 = NPC teleports to end of line at cast resolve
} NPCAbilityDef;

/** Associate movement, aggro, and ability settings with one NPC type. */
typedef struct {
    uint16_t        npc_type_id;

    NPCMovementType movement_type;
    float           move_speed;         // World units per second
    float           preferred_range;    // For MAINTAIN_RANGE: desired distance to target

    float           aggro_range;        // Detection distance
    float           leash_range;        // Max distance from spawn before giving up

    NPCAbilityDef   abilities[MAX_NPC_ABILITIES];
    int             ability_count;
} NPCAIProfile;

// load once during world-server startup
int npc_ai_init(const char* json_path);

void npc_ai_cleanup(void);

/** Bound each NPC's nearest-first candidate search without changing reachability. */
#define NPC_AGGRO_CANDIDATES 16

// update targeting, movement, abilities, and telegraphs at 20 Hz
void npc_ai_tick(NPCWorld* world, TickSnapshot* snap, double delta_time);

// return a registry-owned profile or NULL when absent
const NPCAIProfile* npc_ai_get_profile(uint16_t npc_type_id);

#endif // NPC_AI_H
