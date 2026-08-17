#ifndef COMBAT_SYSTEM_H
#define COMBAT_SYSTEM_H

#include "combat_state.h"
#include "attack_types.h"
#include <stdint.h>

/**
 * @file
 * Declare client combat state transitions driven by frame updates and server events.
 */

void combat_init(CombatState* combat);

void combat_reset(CombatState* combat);

void combat_update(CombatState* combat, float delta_time);

void combat_on_cast_start(CombatState* combat,
                          uint8_t attack_type,
                          float cast_time,
                          float origin_x, float origin_y,
                          float aim_x, float aim_y,
                          uint32_t* target_ids, 
                          uint8_t target_count);

void combat_on_cast_cancel(CombatState* combat);

void combat_on_damage(CombatState* combat,
                      uint32_t target_id,
                      int damage,
                      int is_crit,
                      int is_kill,
                      int is_heal,
                      float target_x,
                      float target_y);

void combat_on_attack_result(CombatState* combat, uint8_t result_code, float cooldown);

int combat_is_casting(const CombatState* combat);

float combat_get_cast_progress(const CombatState* combat);

float combat_get_cast_remaining(const CombatState* combat);

int combat_is_on_cooldown(const CombatState* combat);

float combat_get_cooldown_progress(const CombatState* combat);

#endif // COMBAT_SYSTEM_H
