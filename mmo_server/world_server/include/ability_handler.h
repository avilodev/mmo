/** @file Coordinate server-side ability casts, effects, zones, and resource updates. */

#ifndef ABILITY_HANDLER_H
#define ABILITY_HANDLER_H

#include "ability_def.h"
#include "combat_config.h"
#include "types.h"
#include "player_data.h"

#include <stdint.h>

/** Preserve one player's cast-start state until its resolution time. */
typedef struct {
    uint8_t     is_active;
    double      cast_start_time;
    float       cast_duration;
    uint16_t    ability_id;         // Which ability is being cast
    uint32_t    caster_id;          // Character ID
    int         caster_slot;        // Index in active_players[]
    int         client_fd;

    /** Preserve cast geometry against later player movement. */
    float       origin_x, origin_y;
    float       aim_x, aim_y;
    uint32_t    target_id;          // For single-target abilities
} PendingAbilityCast;

/** Bound concurrently active ability-created zones. */
#define MAX_ZONES 64

/** Track one ability-created zone and its periodic effects. */
typedef struct {
    uint8_t     is_active;
    uint32_t    zone_id;            // Server-assigned
    uint32_t    caster_id;
    uint16_t    ability_id;         // Source ability (for VFX lookup)

    float       pos_x, pos_y;
    float       radius;             // AOE radius
    uint8_t     has_collision;
    int         hp;                 // 0 = indestructible, >0 = destructible
    int         max_hp;

    /** Measure remaining lifetime in seconds. */
    float       duration_remaining;

    /** Retain an immutable copy of effects from the source ability. */
    AbilityEffectDef effects[MAX_ABILITY_EFFECTS];
    uint8_t     effect_count;
    int         healing_per_tick;   // For sanctuary-type zones
    float       tick_timer;         /**< Seconds until the next effect application. */
    float       tick_rate;          /**< Seconds between effect applications. */
} ActiveZone;

// initialize once after loading ability definitions
void ability_handler_init(void);

// validate and queue or reject one cast intent
void ability_handle_cast_intent(NPCWorld* world,
                                int client_fd,
                                uint32_t caster_id,
                                AbilityCastIntentPacket* pkt);

void ability_handle_cast_cancel(int client_fd, uint32_t caster_id);

// update casts, effects, zones, projectiles, and mana at 20 Hz
void ability_tick(NPCWorld* world, double delta_time);

// send after player stats on world entry and level-up
void ability_send_data(int client_fd, ActivePlayer* player);

void ability_handler_cleanup(void);

#endif // ABILITY_HANDLER_H