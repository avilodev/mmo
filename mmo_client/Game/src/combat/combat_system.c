#include "combat_system.h"
#include "attack_defs.h"
#include <string.h>

void combat_init(CombatState* combat) {
    memset(combat, 0, sizeof(CombatState));
}

void combat_reset(CombatState* combat) {
    memset(combat, 0, sizeof(CombatState));
}

void combat_update(CombatState* combat, float delta_time) {
    // Update cast timer
    if (combat->is_casting) {
        combat->cast_elapsed += delta_time;
        
        if (combat->cast_elapsed >= combat->cast_duration) {
            combat->is_casting = 0;
        }
    }
    
    // Update cooldown
    if (combat->cooldown_remaining > 0.0f) {
        combat->cooldown_remaining -= delta_time;
        if (combat->cooldown_remaining < 0.0f) {
            combat->cooldown_remaining = 0.0f;
        }
    }
    
    // Update damage events (age them, deactivate old ones)
    for (int i = 0; i < MAX_DAMAGE_EVENTS; i++) {
        DamageEvent* evt = &combat->damage_events[i];
        if (!evt->active) continue;
        
        evt->age += delta_time;
        
        if (evt->age > 2.0f) {
            evt->active = 0;
        }
    }
}

void combat_on_cast_start(CombatState* combat,
                          uint8_t attack_type,
                          float cast_time,
                          float origin_x, float origin_y,
                          float aim_x, float aim_y,
                          uint32_t* target_ids, uint8_t target_count) {
    
    combat->is_casting = 1;
    combat->cast_elapsed = 0.0f;
    combat->cast_duration = cast_time;
    combat->attack_type = (AttackType)attack_type;
    
    combat->origin_x = origin_x;
    combat->origin_y = origin_y;
    combat->aim_x = aim_x;
    combat->aim_y = aim_y;
    
    combat->target_count = target_count;
    if (target_count > MAX_COMBAT_TARGETS) {
        combat->target_count = MAX_COMBAT_TARGETS;
    }
    for (int i = 0; i < combat->target_count; i++) {
        combat->target_ids[i] = target_ids[i];
    }
    
    const AttackDef* def = attack_def_get_by_type(combat->attack_type);
    combat->range = def->range;
    combat->radius = def->radius;
    combat->cone_angle = def->cone_angle;
    combat->line_width = def->line_width;
    combat->color_r = def->color_r;
    combat->color_g = def->color_g;
    combat->color_b = def->color_b;
    combat->color_a = def->color_a;
}

void combat_on_cast_cancel(CombatState* combat) {
    combat->is_casting = 0;
    combat->cast_elapsed = 0.0f;
}

void combat_on_damage(CombatState* combat,
                      uint32_t target_id,
                      int damage,
                      int is_crit,
                      int is_kill,
                      int is_heal,
                      float target_x,
                      float target_y) {

    for (int i = 0; i < MAX_DAMAGE_EVENTS; i++) {
        DamageEvent* evt = &combat->damage_events[i];
        if (evt->active) continue;

        evt->active    = 1;
        evt->target_id = target_id;
        evt->amount    = damage;
        evt->is_kill   = is_kill;
        evt->is_crit   = is_crit;
        evt->is_heal   = is_heal;
        evt->world_x   = target_x;
        evt->world_y   = target_y;
        evt->age       = 0.0f;
        break;
    }

    if (!is_heal) combat->is_casting = 0;
}

void combat_on_attack_result(CombatState* combat, uint8_t result_code, float cooldown) {
    if (result_code == 0) {  // ATTACK_RESULT_OK
        combat->cooldown_remaining = cooldown;
        combat->cooldown_total = cooldown;
    }
}

int combat_is_casting(const CombatState* combat) {
    return combat->is_casting;
}

float combat_get_cast_progress(const CombatState* combat) {
    if (!combat->is_casting || combat->cast_duration <= 0.0f) {
        return 0.0f;
    }
    float progress = combat->cast_elapsed / combat->cast_duration;
    return (progress > 1.0f) ? 1.0f : progress;
}

float combat_get_cast_remaining(const CombatState* combat) {
    if (!combat->is_casting || combat->cast_duration <= 0.0f) {
        return 0.0f;
    }
    
    float remaining = combat->cast_duration - combat->cast_elapsed;
    return (remaining < 0.0f) ? 0.0f : remaining;
}

int combat_is_on_cooldown(const CombatState* combat) {
    return combat->cooldown_remaining > 0.0f;
}

float combat_get_cooldown_progress(const CombatState* combat) {
    if (combat->cooldown_total <= 0.0f) {
        return 0.0f;
    }

    float progress = combat->cooldown_remaining / combat->cooldown_total;
    
    if (progress < 0.0f) return 0.0f;
    if (progress > 1.0f) return 1.0f;
    
    return progress;
}