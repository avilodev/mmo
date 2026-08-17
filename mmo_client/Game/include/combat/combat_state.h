#ifndef COMBAT_STATE_H
#define COMBAT_STATE_H

#include "attack_types.h"
#include <stdint.h>

#define MAX_COMBAT_TARGETS 16
#define MAX_DAMAGE_EVENTS  8

/** Track one timed combat-number event in world space. */
typedef struct {
    uint32_t target_id;
    float    world_x;
    float    world_y;
    int      amount;
    int      is_kill;
    int      is_crit;
    int      is_heal;
    float    age;           /**< Seconds since receipt. */
    int      active;
} DamageEvent;

/** Aggregate current cast geometry, targets, presentation, and cooldown state. */
typedef struct {
    int         is_casting;
    float       cast_elapsed;       /**< Elapsed cast time in seconds. */
    float       cast_duration;      /**< Total cast duration in seconds. */
    AttackType  attack_type;
    
    /** Server-provided world-space cast geometry. */
    float       origin_x;
    float       origin_y;
    float       aim_x;
    float       aim_y;
    
    uint32_t    target_ids[MAX_COMBAT_TARGETS];
    uint8_t     target_count;
    
    /** Attack-definition geometry and color used for rendering. */
    float       range;
    float       radius;
    float       cone_angle;
    float       line_width;
    float       color_r, color_g, color_b, color_a;
    
    DamageEvent damage_events[MAX_DAMAGE_EVENTS];
    
    float       cooldown_remaining; /**< Remaining attack cooldown in seconds. */
    float       cooldown_total;     /**< Total attack cooldown in seconds. */
    
} CombatState;

#endif // COMBAT_STATE_H
