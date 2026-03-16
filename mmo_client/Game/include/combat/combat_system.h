#ifndef COMBAT_SYSTEM_H
#define COMBAT_SYSTEM_H

#include "combat_state.h"
#include "attack_types.h"
#include <stdint.h>

// ============================================================================
// COMBAT SYSTEM - Main API for combat state management
// ============================================================================

// Initialize combat state (call once at game start)
void combat_init(CombatState* combat);

// Reset combat state (call when changing characters or disconnecting)
void combat_reset(CombatState* combat);

// Update combat state (call every frame)
// delta_time in seconds
void combat_update(CombatState* combat, float delta_time);

// ============================================================================
// EVENT HANDLERS - Call these when network packets arrive
// ============================================================================

// Handle PACKET_CAST_START_V2 from server
void combat_on_cast_start(CombatState* combat,
                          uint8_t attack_type,
                          float cast_time,
                          float origin_x, float origin_y,
                          float aim_x, float aim_y,
                          uint32_t* target_ids, 
                          uint8_t target_count);

// Handle PACKET_CAST_CANCEL from server
void combat_on_cast_cancel(CombatState* combat);

// Handle PACKET_DAMAGE_V2 / PACKET_ABILITY_EFFECT from server
void combat_on_damage(CombatState* combat,
                      uint32_t target_id,
                      int damage,
                      int is_crit,
                      int is_kill,
                      int is_heal,
                      float target_x,
                      float target_y);

// Handle PACKET_ATTACK_RESULT from server (for cooldown tracking)
void combat_on_attack_result(CombatState* combat, uint8_t result_code, float cooldown);

// ============================================================================
// STATE QUERIES
// ============================================================================

// Is currently casting an attack?
int combat_is_casting(const CombatState* combat);

// Get cast progress (0.0 to 1.0)
float combat_get_cast_progress(const CombatState* combat);

// Get remaining cast time in seconds
float combat_get_cast_remaining(const CombatState* combat);

// Is attack on cooldown?
int combat_is_on_cooldown(const CombatState* combat);

// Get cooldown progress (0.0 = ready, 1.0 = just started cooldown)
float combat_get_cooldown_progress(const CombatState* combat);

#endif // COMBAT_SYSTEM_H