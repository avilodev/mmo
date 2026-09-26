/** @file Coordinate server-side ability casts, effects, zones, and resource updates. */

#ifndef ABILITY_HANDLER_H
#define ABILITY_HANDLER_H

#include "npc_snapshot.h"
#include "npc_world.h"
#include "tick_snapshot.h"
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

/** Size the ground-zone pool when a world configuration does not say.
 *
 * One pool serves players and, once V6 lands, NPCs -- a Firebrand lays five
 * zones per cast and a Rot Crawler one per tick, so a fixed global array is a
 * content limit rather than a memory decision. Heap-allocated from the world's
 * `.conf` (`zone_capacity`).
 *
 * Note the client renders at most MAX_ZONES of its own (32 today). Raising this
 * lets more zones *exist*; how many are *drawn* is a separate client bound, and
 * a zone that is not drawn still applies its effects.
 */
#define ZONE_CAPACITY_DEFAULT 256

/** Refuse absurd configured capacities rather than trying to allocate them. */
#define ZONE_CAPACITY_MAX   16384

/** Track one ability-created zone and its periodic effects.
 *
 * Lives here rather than in zone_owner.h so that the pool's owner and the
 * ability path that started it agree on one struct. zone_owner.c is the only
 * code that writes it.
 */
typedef struct {
    uint8_t     is_active;
    uint32_t    zone_id;            // Server-assigned
    uint32_t    caster_id;
    uint8_t     owner_type;         /**< ZoneOwnerType: whose zone this is. */
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

/** Initialize pending casts and the ground-zone pool.
 *
 * @param zone_capacity  Requested zone slots; 0 takes ZONE_CAPACITY_DEFAULT and
 *                       the value is clamped to ZONE_CAPACITY_MAX.
 * @param zone_per_owner Zones one caster may hold; 0 takes the default.
 * @return               1 on success, or 0 when allocation fails.
 */
int ability_handler_init(int zone_capacity, int zone_per_owner);

/** Report the zone pool's allocated capacity. */
int ability_zone_capacity(void);

// validate and queue or reject one cast intent
void ability_handle_cast_intent(NPCWorld* world,
                                int client_fd,
                                uint32_t caster_id,
                                AbilityCastIntentPacket* pkt);

void ability_handle_cast_cancel(int client_fd, uint32_t caster_id);

// update casts, effects, zones, projectiles, and mana at 20 Hz
/** Resolve elapsed casts, zone ticks, and regeneration on the gameplay thread.
 *
 * @param world  NPC pool that owns the entities damage and effects are applied to.
 * @param npcs  This tick's NPC snapshot, used to narrow targets before locking.
 * @param delta_time  Elapsed tick time in seconds.
 */
/** Advance casts, effects, zones, and resource regeneration for one gameplay tick.
 *
 * @param players  The pass's player snapshot, used to find who is standing in a
 *                 zone; may be NULL, in which case zones affect NPCs only.
 * @param npcs     The pass's NPC snapshot, used for the same question about NPCs.
 */
void ability_tick(NPCWorld* world, TickSnapshot* players,
                  NPCTickSnapshot* npcs, double delta_time);

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