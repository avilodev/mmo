// ============================================================================
// ability_handler.h — Server-side ability casting, resolution, and effects
//
// Replaces combat_handle_attack_intent for ability-based casts.
// Called from routes.c and ticked from combat_update_thread.
// ============================================================================

#ifndef ABILITY_HANDLER_H
#define ABILITY_HANDLER_H

#include "ability_def.h"
#include "combat_config.h"
#include "types.h"
#include "player_data.h"

#include <stdint.h>

// ---------------------------------------------------------------------------
// Pending ability cast — one per player, tracked in a global array.
// When cast_time elapses, ability_tick() resolves it.
// ---------------------------------------------------------------------------

typedef struct {
    uint8_t     is_active;
    double      cast_start_time;
    float       cast_duration;
    uint16_t    ability_id;         // Which ability is being cast
    uint32_t    caster_id;          // Character ID
    int         caster_slot;        // Index in active_players[]
    int         client_fd;

    // Snapshot at cast start
    float       origin_x, origin_y;
    float       aim_x, aim_y;
    uint32_t    target_id;          // For single-target abilities
} PendingAbilityCast;

// ---------------------------------------------------------------------------
// Active zone entity — spawned by abilities like Rock Wall, Sanctuary, Mud Pit
// ---------------------------------------------------------------------------

#define MAX_ZONES 64

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

    float       duration_remaining;

    // Zone effects (copied from ability def)
    AbilityEffectDef effects[MAX_ABILITY_EFFECTS];
    uint8_t     effect_count;
    int         healing_per_tick;   // For sanctuary-type zones
    float       tick_timer;         // Time until next effect application
    float       tick_rate;          // How often effects reapply (1.0 = every second)
} ActiveZone;

// ============================================================================
// API
// ============================================================================

// Initialize the ability handler system. Call once at startup after abilities_init().
void ability_handler_init(void);

// Handle an incoming ABILITY_CAST_INTENT packet from a player.
// Validates everything server-side, then either queues a pending cast
// or rejects with a reason packet.
void ability_handle_cast_intent(NPCWorld* world,
                                int client_fd,
                                uint32_t caster_id,
                                AbilityCastIntentPacket* pkt);

// Handle ability cast cancel from a player.
void ability_handle_cast_cancel(int client_fd, uint32_t caster_id);

// Per-tick update. Call from combat_update_thread at 20Hz.
// Resolves completed casts, ticks status effects, ticks zones,
// ticks projectiles, regens mana.
void ability_tick(NPCWorld* world, double delta_time);

// Send the player's current ability bar data (IDs, names, cooldowns, costs) to client.
// Call after player_send_stats() on world entry and after level-up.
void ability_send_data(int client_fd, ActivePlayer* player);

// Cleanup
void ability_handler_cleanup(void);

#endif // ABILITY_HANDLER_H