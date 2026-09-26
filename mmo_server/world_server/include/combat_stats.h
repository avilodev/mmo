/** @file Provide shared formulas turning character attributes into combat numbers.
 *
 * These are the meanings of the eleven stats, in one place. The scaling constants
 * they read come from progression.json rather than from this header, so retuning
 * what a point of Precision is worth is a data change.
 *
 * There is deliberately no evasion or dodge here. Mitigation is Armor and
 * percentage reducers only; an attack that connects always lands.
 */

#ifndef COMBAT_STATS_H
#define COMBAT_STATS_H

#include "progression.h"
#include "protocol.h"

#include <math.h>
#include <stdlib.h>

/** Convert one scaling attribute point into one percent bonus damage. */
#define STAT_DAMAGE_DIVISOR 100.0f

/** Calculate an ability's damage multiplier from the attribute it scales with.
 *
 * @param damage_stat  A StatId, or a value outside 0..STAT_COUNT for no scaling.
 * @param stats        The caster's attributes, indexed by StatId.
 * @return             The scaled multiplier, or 1.0 when the ability does not scale.
 */
static inline float combat_ability_damage_mult(int damage_stat, const int* stats) {
    if (!stats || damage_stat < 0 || damage_stat >= STAT_COUNT) return 1.0f;
    return 1.0f + ((float)stats[damage_stat] / STAT_DAMAGE_DIVISOR);
}

/** Roll a critical hit from a Precision score.
 *
 * @return Nonzero for a critical hit, otherwise zero.
 */
static inline int combat_check_crit(int precision) {
    if (precision <= 0) return 0;

    const ProgressionConfig* c = progression_config();
    double chance = precision * c->crit_chance_per_precision;
    if (chance > c->crit_chance_cap) chance = c->crit_chance_cap;

    return (rand() % 10000) < (int)(chance * 100.0);
}

/** Return the damage multiplier a critical hit applies, from a Ferocity score.
 *
 * @return At least 1.0, so zero Ferocity still crits for full damage.
 */
static inline float combat_crit_multiplier(int ferocity) {
    if (ferocity < 0) ferocity = 0;
    return 1.0f + (float)(ferocity * progression_config()->crit_damage_per_ferocity);
}

/** Return the movement speed a Dexterity score adds, in world pixels per second. */
static inline float combat_move_speed_bonus(int dexterity) {
    if (dexterity < 0) dexterity = 0;
    return (float)(dexterity * progression_config()->move_speed_per_dexterity);
}

/** Configure the Dexterity contribution to attack and cast speed, and its cap. */
#define DEX_HASTE_DIVISOR 200.0f
#define DEX_HASTE_CAP     0.40f

/** Calculate the cooldown and cast-time multiplier a Dexterity score grants.
 *
 * Dexterity is attack and cast speed, so it shortens both. The cap keeps a very
 * high roll from collapsing cooldowns to nothing.
 *
 * @return A multiplier no lower than one minus DEX_HASTE_CAP.
 */
static inline float combat_haste_multiplier(int dexterity) {
    if (dexterity < 0) dexterity = 0;
    float reduction = (float)dexterity / DEX_HASTE_DIVISOR;
    if (reduction > DEX_HASTE_CAP) reduction = DEX_HASTE_CAP;
    return 1.0f - reduction;
}

/** Convert one point of Focus into this fraction of bonus healing. */
#define FOCUS_HEAL_DIVISOR 100.0f

/** Calculate the healing multiplier a Focus score grants.
 *
 * @return At least 1.0, including for a nonpositive score.
 */
static inline float combat_healing_multiplier(int focus) {
    if (focus <= 0) return 1.0f;
    return 1.0f + ((float)focus / FOCUS_HEAL_DIVISOR);
}

/** Calculate a resource's regeneration per second.
 *
 * Rage is excluded on purpose: it inverts the usual pool, building through combat
 * and decaying out of it, so it regenerates through combat_rage_gain() instead.
 *
 * @param type        The pool being regenerated.
 * @param stat_value  The score of the stat governing that pool.
 * @return            Points restored per second, or zero for rage and for no pool.
 */
static inline float combat_resource_regen_rate(int type, int stat_value) {
    const ProgressionConfig* c = progression_config();
    if (stat_value < 0) stat_value = 0;

    switch (type) {
        case RESOURCE_MANA:
            return (float)(c->resource_regen_base + stat_value * c->mana_regen_per_focus);
        case RESOURCE_STAMINA:
            return (float)(c->resource_regen_base + stat_value * c->stamina_regen_per_capacity);
        default:
            return 0.0f;
    }
}

/** Calculate the rage gained from one combat event.
 *
 * @param damage_dealt  Damage this character dealt; zero when reacting to a hit.
 * @param damage_taken  Damage this character took; zero when acting.
 * @return              Rage points gained, never negative.
 */
static inline float combat_rage_gain(int damage_dealt, int damage_taken) {
    const ProgressionConfig* c = progression_config();
    if (damage_dealt < 0) damage_dealt = 0;
    if (damage_taken < 0) damage_taken = 0;

    return (float)(damage_dealt * c->rage_per_damage_dealt +
                   damage_taken * c->rage_per_damage_taken);
}

/** Return the Animal Form power multiplier a Ferality score grants.
 *
 * This is the single knob controlling the Human/Animal power gap. Human Form does
 * not apply it, which — together with the race passive being Animal-Form-only — is
 * the whole of what makes Human Form weaker.
 *
 * @return At least 1.0.
 */
static inline float combat_ferality_multiplier(int ferality) {
    if (ferality < 0) ferality = 0;
    return 1.0f + (float)(ferality * progression_config()->animal_power_per_ferality);
}

/** Return the ability power multiplier for a form.
 *
 * @param form      The active PlayerForm.
 * @param ferality  The character's Ferality score.
 * @return          The Ferality multiplier in Animal Form, or exactly 1.0 in Human Form.
 */
static inline float combat_form_power_multiplier(int form, int ferality) {
    return (form == FORM_ANIMAL) ? combat_ferality_multiplier(ferality) : 1.0f;
}

/** Configure out-of-combat health regeneration and its inactivity threshold. */
#define HP_REGEN_BASE 0.5f
#define HP_REGEN_HP_RATIO 0.005f
#define OOC_THRESHOLD_SECONDS 5.0

/** Calculate out-of-combat health regenerated per second.
 *
 * @return The health regeneration rate per second.
 */
static inline float combat_hp_regen_rate(int max_health) {
    return HP_REGEN_BASE + ((float)max_health * HP_REGEN_HP_RATIO);
}

#endif // COMBAT_STATS_H
