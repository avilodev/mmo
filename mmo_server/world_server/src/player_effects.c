/**
 * @file
 * Resolve a player's active effects, race passive, and form into combat state.
 *
 * The organising decision here is that damage modifiers are derived on demand rather
 * than maintained incrementally. Attribute buffs are the exception — combat reads
 * attributes constantly, so those are folded into the player's stat array — but they
 * are rebuilt from the race curve every time anything changes, never adjusted in
 * place. Both choices exist for the same reason: a value that is only ever recomputed
 * cannot drift out of step with the effects that produced it.
 */

#include "player_effects.h"
#include "log.h"

#include "class_stats.h"
#include "combat_stats.h"
#include "items_database.h"
#include "progression.h"

#include <stdio.h>
#include <string.h>

/**
 * Return the race passive in force for a player, or NULL when none applies.
 *
 * Human Form applies no passive for any race. That single gate is what makes Human
 * Form identical across all ten races, so it lives here rather than being repeated
 * at each place a passive might be read.
 */
const RacePassive* player_active_passive(const ActivePlayer* player) {
    if (!player || player->form != FORM_ANIMAL) return NULL;

    const RaceDef* race = race_get(player->race_id);
    if (!race || !race->passive.modifier_count) return NULL;

    return &race->passive;
}

/**
 * Collect every damage modifier applying to a player right now.
 */
void player_collect_modifiers(const ActivePlayer* player, int ally_count,
                              DamageModifiers* out) {
    if (!out) return;
    damage_mods_reset(out);
    if (!player) return;

    /* Armor is a character attribute, so it is always in play regardless of form. */
    damage_mods_add_armor(out, player->stats[STAT_ARMOR]);

    const RacePassive* passive = player_active_passive(player);
    if (passive) {
        damage_mods_add_taken(out,
            race_passive_modifier(passive, PASSIVE_MOD_DAMAGE_TAKEN, ally_count));
        damage_mods_add_dealt(out,
            race_passive_modifier(passive, PASSIVE_MOD_DAMAGE_DEALT, ally_count));
        damage_mods_add_armor(out,
            (int)race_passive_modifier(passive, PASSIVE_MOD_ARMOR, ally_count));
    }

    for (int i = 0; i < MAX_ACTIVE_EFFECTS; i++) {
        if (!player->active_effects[i].active) continue;

        int value = player->active_effects[i].value;
        switch (player->active_effects[i].effect_type) {
            case EFFECT_DAMAGE_TAKEN:
                damage_mods_add_taken(out, effect_permille_to_fraction(value));
                break;
            case EFFECT_DAMAGE_DEALT:
                damage_mods_add_dealt(out, effect_permille_to_fraction(value));
                break;
            case EFFECT_MARK:
                /* A mark raises damage taken, so it is a negative reduction --
                 * the same sign convention npc_effects.c states. Rogue Hawk's
                 * whole design is that the mark is worth removing before the
                 * burst lands. */
                damage_mods_add_taken(out, -effect_permille_to_fraction(value));
                break;
            default:
                break;
        }
    }
}

/**
 * Report whether a player's actions are locked.
 *
 * @return Nonzero while an EFFECT_CHANNEL or EFFECT_STUN is active.
 */
int player_is_action_locked(const ActivePlayer* player) {
    if (!player) return 0;

    for (int i = 0; i < MAX_ACTIVE_EFFECTS; i++) {
        if (!player->active_effects[i].active) continue;

        uint8_t type = player->active_effects[i].effect_type;
        if (type == EFFECT_CHANNEL || type == EFFECT_STUN) return 1;
    }
    return 0;
}

/**
 * Report whether a player's movement is locked.
 *
 * @return Nonzero while an EFFECT_ROOT, EFFECT_CHANNEL or EFFECT_STUN is active.
 */
int player_is_movement_locked(const ActivePlayer* player) {
    if (!player) return 0;

    for (int i = 0; i < MAX_ACTIVE_EFFECTS; i++) {
        if (!player->active_effects[i].active) continue;

        uint8_t type = player->active_effects[i].effect_type;
        if (type == EFFECT_ROOT || type == EFFECT_CHANNEL || type == EFFECT_STUN) return 1;
    }
    return 0;
}

/* --- The five effects the NPC kits introduce -------------------------------
 *
 * Each is a predicate over a gate that already exists rather than a new
 * subsystem, which is what V11 claimed and what these four functions are.
 */

/** Report whether a player may change form.
 *
 * Chain-breaker's chain and Silencer's bubble both mean "stay as you are" --
 * which is the harshest thing either faction can say to a Blessed, and the
 * reason form-lock is a distinct effect rather than a stun.
 */
int player_is_form_locked(const ActivePlayer* player) {
    if (!player) return 0;
    for (int i = 0; i < MAX_ACTIVE_EFFECTS; i++) {
        if (!player->active_effects[i].active) continue;
        if (player->active_effects[i].effect_type == EFFECT_FORM_LOCK) return 1;
    }
    return 0;
}

float player_blind_spread(const ActivePlayer* player) {
    if (!player) return 0.0f;
    float widest = 0.0f;
    for (int i = 0; i < MAX_ACTIVE_EFFECTS; i++) {
        if (!player->active_effects[i].active) continue;
        if (player->active_effects[i].effect_type != EFFECT_BLIND) continue;
        /* `value` is the half-angle in degrees; a blind with no value stated is
         * still a blind, so it takes a usable default rather than none. */
        float v = player->active_effects[i].value > 0
                ? (float)player->active_effects[i].value : 30.0f;
        if (v > widest) widest = v;
    }
    return widest;
}

uint32_t player_fear_source(const ActivePlayer* player) {
    if (!player) return 0;
    for (int i = 0; i < MAX_ACTIVE_EFFECTS; i++) {
        if (!player->active_effects[i].active) continue;
        if (player->active_effects[i].effect_type == EFFECT_FEAR)
            return player->active_effects[i].source_id;
    }
    return 0;
}

uint32_t player_charm_source(const ActivePlayer* player) {
    if (!player) return 0;
    for (int i = 0; i < MAX_ACTIVE_EFFECTS; i++) {
        if (!player->active_effects[i].active) continue;
        if (player->active_effects[i].effect_type == EFFECT_CHARM)
            return player->active_effects[i].source_id;
    }
    return 0;
}

/** Return the ability power multiplier for a player's current form. */
float player_form_power(const ActivePlayer* player) {
    if (!player) return 1.0f;
    return combat_form_power_multiplier(player->form, player->stats[STAT_FERALITY]);
}

/**
 * Report whether a form and slot index address a real hotbar slot.
 */
static int valid_slot(uint8_t form, int slot) {
    return form < FORM_COUNT && slot >= 0 && slot < MAX_ABILITY_SLOTS;
}

/**
 * Start a cooldown on one hotbar slot.
 */
void player_start_cooldown(ActivePlayer* player, uint8_t form, int slot,
                           double now, double duration) {
    if (!player || !valid_slot(form, slot)) return;
    if (duration < 0.0) duration = 0.0;

    player->ability_ready_at[form][slot] = now + duration;
}

/**
 * Return the seconds remaining on one hotbar slot's cooldown.
 *
 * @return Zero when the slot is ready, or when the form or slot is out of range.
 */
double player_cooldown_remaining(const ActivePlayer* player, uint8_t form, int slot,
                                 double now) {
    if (!player || !valid_slot(form, slot)) return 0.0;

    double remaining = player->ability_ready_at[form][slot] - now;
    return remaining > 0.0 ? remaining : 0.0;
}

/**
 * Report whether one hotbar slot may be used.
 */
int player_slot_is_ready(const ActivePlayer* player, uint8_t form, int slot, double now) {
    return player_cooldown_remaining(player, form, slot, now) <= 0.0;
}

/**
 * Return the stat sizing a player's resource pool, or STAT_FOCUS when it has none.
 */
static StatId resource_stat_for(const ActivePlayer* player) {
    switch (player->resource_type) {
        case RESOURCE_RAGE:    return STAT_ENDURANCE;
        case RESOURCE_STAMINA: return STAT_STAMINA_CAPACITY;
        default:               return STAT_FOCUS;
    }
}

/**
 * Recompute a player's attributes from the race curve, gear, and active buffs.
 *
 * Order matters: the race curve is the base, gear adds to it, and buffs add last, so
 * a buff never multiplies a bonus it should not see. Nothing is subtracted anywhere,
 * which is why a buff applied twice and removed once cannot leave a residue.
 */
void player_recompute_stats(ActivePlayer* player) {
    if (!player) return;

    DerivedStats base;
    if (!class_stats_compute(player->race_id, player->level, &base)) {
        LOG_INFO("[STATS] Race %u is not in the registry; player %u keeps its last stats",
                 player->race_id, player->character_id);
        return;
    }

    memcpy(player->base_stats, base.stats, sizeof(player->base_stats));
    memcpy(player->stats,      base.stats, sizeof(player->stats));

    player->move_speed    = base.move_speed;
    player->weapon_damage = 0;

    /* Human Form is cooldown-only and carries no pool at all. */
    player->resource_type = (player->form == FORM_HUMAN) ? RESOURCE_NONE : base.resource_type;

    for (int i = 0; i < EQUIP_SLOTS; i++) {
        if (i == EQUIP_BLESSING) continue;   /* the blessing carries no item stats */
        if (player->equipment[i].instance_id == 0) continue;

        const ItemDefinition* item = item_get(player->equipment[i].item_id);
        if (!item) continue;

        if (item->type == ITEM_TYPE_WEAPON) {
            player->weapon_damage += (int)item->damage;
        }
        if (item->type == ITEM_TYPE_ARMOR || item->type == ITEM_TYPE_SHIELD) {
            player->stats[STAT_ARMOR] += (int)item->defense;
        }
        for (int stat = 0; stat < STAT_COUNT; stat++) {
            player->stats[stat] += item->bonus_stats[stat];
        }
    }

    for (int i = 0; i < MAX_ACTIVE_EFFECTS; i++) {
        if (!player->active_effects[i].active) continue;
        if (player->active_effects[i].effect_type != EFFECT_BUFF) continue;

        int value = player->active_effects[i].value;
        StatType target = (StatType)(int8_t)player->active_effects[i].buff_stat;

        if (stat_target_is_attribute(target)) {
            player->stats[target] += value;
        } else if (target == STAT_TARGET_MOVE_SPEED) {
            player->move_speed += (float)value;
        } else if (target == STAT_TARGET_WEAPON_DAMAGE) {
            player->weapon_damage += value;
        }
    }

    /* Dexterity's contribution to speed is applied once, after every source of
     * Dexterity is in — gear and buffs included. */
    player->move_speed += combat_move_speed_bonus(player->stats[STAT_DEXTERITY]);

    player->max_health = class_stats_health_for_vitality(player->stats[STAT_VITALITY]);
    player->max_resource = class_stats_resource_for_stat(
        (ResourceType)player->resource_type, player->stats[resource_stat_for(player)]);

    if (player->health > player->max_health) player->health = player->max_health;

    /* Human Form has no pool rather than an empty one, so the stored value is left
     * alone while the character is out of Animal Form. Clamping here would mean a
     * tank lost every point of rage by swapping out and back — which is exactly the
     * kind of thing a swap must not do. */
    if (player->max_resource > 0 && player->resource > player->max_resource) {
        player->resource = player->max_resource;
    }
    if (player->resource < 0) player->resource = 0;
}

/**
 * Add rage earned from one combat event, clamped to the pool.
 */
void player_add_rage(ActivePlayer* player, int damage_dealt, int damage_taken) {
    if (!player || player->resource_type != RESOURCE_RAGE) return;

    player->resource += (int)combat_rage_gain(damage_dealt, damage_taken);
    if (player->resource > player->max_resource) player->resource = player->max_resource;
}

/**
 * Install one effect in a player's first free slot.
 *
 * @return 1 when the effect was installed or applied, or 0 when no slot was free.
 */
int player_effect_apply(ActivePlayer* player, const AbilityEffectDef* effect,
                        uint32_t source_id) {
    if (!player || !effect) return 0;

    /* Instant effects change a pool once and have nothing to expire, so they never
     * occupy a slot — which matters, because there are only eight. */
    if (effect->type == EFFECT_RESOURCE) {
        int restored = percent_of_max(player->max_resource,
                                      effect_permille_to_fraction(effect->value));
        player->resource += restored;
        if (player->resource > player->max_resource) player->resource = player->max_resource;
        return 1;
    }

    for (int i = 0; i < MAX_ACTIVE_EFFECTS; i++) {
        if (player->active_effects[i].active) continue;

        player->active_effects[i].active             = 1;
        player->active_effects[i].effect_type        = (uint8_t)effect->type;
        player->active_effects[i].buff_stat          = (uint8_t)(int8_t)effect->stat;
        player->active_effects[i].value              = effect->value;
        player->active_effects[i].duration_remaining = effect->duration;
        player->active_effects[i].tick_rate          = effect->tick_rate;
        player->active_effects[i].tick_remaining     = effect->tick_rate;
        player->active_effects[i].source_id          = source_id;

        /* An attribute buff changes numbers combat reads on every hit, so fold it in
         * now rather than deriving it later. Damage-taken and damage-dealt modifiers
         * are deliberately not folded in — they are collected on demand. */
        if (effect->type == EFFECT_BUFF) player_recompute_stats(player);
        return 1;
    }

    LOG_INFO("[EFFECT] No free effect slot on player %u; effect %d dropped",
             player->character_id, (int)effect->type);
    return 0;
}
