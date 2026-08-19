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

/** Rebuild both forms' hotbars from the race registry and the player's level.
 *
 * Both hotbars are derived, never stored: with exactly five abilities available for
 * five slots there is no loadout to build, so there is no table to migrate and no
 * ability-book UI to write. Growing the Human Form pool later becomes a data change
 * plus a UI, not a re-architecture.
 *
 * Cooldowns are deliberately left alone. They are absolute expiry instants, so a
 * hotbar rebuild — from a level-up or a form swap — cannot clear one.
 *
 * The caller must hold the player's lock.
 */
void ability_refresh_hotbars(ActivePlayer* player);

/** Send the ability bar for one form.
 *
 * @param form  The PlayerForm whose five slots to send.
 */
void ability_send_form_data(int client_fd, ActivePlayer* player, uint8_t form);

/** Send the ability bar for the player's active form. */
void ability_send_data(int client_fd, ActivePlayer* player);

/** Handle a client's request to swap forms.
 *
 * Validates the requested form and the shared swap cooldown, then replies with the
 * authoritative form, resource pool, and per-slot cooldowns either way.
 */
void ability_handle_form_swap(int client_fd, uint32_t caster_id, uint8_t requested_form);

/** Return the seconds remaining on one hotbar slot's cooldown.
 *
 * @return Zero when the slot is ready, out of range, or empty.
 */
float ability_slot_cooldown_remaining(const ActivePlayer* player, uint8_t form, int slot);

void ability_handler_cleanup(void);

#endif // ABILITY_HANDLER_H