#ifndef COMBAT_STATE_H
#define COMBAT_STATE_H

#include "attack_types.h"
#include <stdint.h>

#define MAX_COMBAT_TARGETS 16
#define MAX_DAMAGE_EVENTS  8

// Single damage event for floating text
typedef struct {
    uint32_t target_id;
    float    world_x;
    float    world_y;
    int      amount;
    int      is_kill;
    float    age;           // Seconds since event
    int      active;
    int      is_crit;
} DamageEvent;

// Main combat state
typedef struct {
    // Cast state
    int         is_casting;
    float       cast_elapsed;
    float       cast_duration;
    AttackType  attack_type;
    
    // Cast geometry (from server)
    float       origin_x;
    float       origin_y;
    float       aim_x;
    float       aim_y;
    
    // Targets
    uint32_t    target_ids[MAX_COMBAT_TARGETS];
    uint8_t     target_count;
    
    // Visual parameters (looked up from attack def)
    float       range;
    float       radius;
    float       cone_angle;
    float       line_width;
    float       color_r, color_g, color_b, color_a;
    
    // Damage events for floating text
    DamageEvent damage_events[MAX_DAMAGE_EVENTS];
    
    // Cooldown tracking
    float       cooldown_remaining;
    int         cooldown_total;
    
} CombatState;

#endif // COMBAT_STATE_H