/** @file Provide shared inline formulas for world-server combat statistics. */

#ifndef COMBAT_STATS_H
#define COMBAT_STATS_H

#include <stdlib.h>
#include <math.h>

/** Convert one scaling attribute point into one percent bonus damage. */
#define STAT_DAMAGE_DIVISOR 100.0f

/** Calculate an ability multiplier from its configured StatType value.
 *
 * @return The scaled multiplier, or 1.0 for an unsupported stat.
 */
static inline float combat_ability_damage_mult(int damage_stat,
                                                int strength, int agility,
                                                int intelligence, int wisdom) {
    int val;
    switch (damage_stat) {
        case 4: val = strength;     break; // STAT_STRENGTH
        case 5: val = agility;      break; // STAT_AGILITY
        case 6: val = intelligence; break; // STAT_INTELLIGENCE
        case 7: val = wisdom;       break; // STAT_WISDOM
        default: return 1.0f;             // STAT_NONE or unknown = no scaling
    }
    return 1.0f + ((float)val / STAT_DAMAGE_DIVISOR);
}

/** Apply diminishing defense reduction while preserving one minimum damage.
 *
 * @return The reduced positive damage value.
 */
static inline int combat_apply_defense(int raw_damage, int defense) {
    if (defense <= 0) return raw_damage;
    int reduced = (raw_damage * 100) / (100 + defense);
    return (reduced < 1) ? 1 : reduced;  // enforce one damage minimum
}

/** Cap the diminishing evasion formula at a percentage chance. */
#define EVASION_CAP_PERCENT 60

/** Roll the capped diminishing evasion chance.
 *
 * @return Nonzero when the attack is dodged, otherwise zero.
 */
static inline int combat_check_evasion(int evasion) {
    if (evasion <= 0) return 0;
    int dodge_chance = (evasion * 100) / (evasion + 100);
    if (dodge_chance > EVASION_CAP_PERCENT) dodge_chance = EVASION_CAP_PERCENT;
    int roll = rand() % 100;
    return (roll < dodge_chance) ? 1 : 0;
}

/** Configure diminishing luck scaling and the critical damage multiplier. */
#define CRIT_LUCK_DIVISOR    150.0f
#define CRIT_DAMAGE_MULTIPLIER 1.75f

/** Roll the diminishing critical-hit chance derived from luck.
 *
 * @return Nonzero for a critical hit, otherwise zero.
 */
static inline int combat_check_crit(int luck) {
    if (luck <= 0) return 0;
    float crit_chance = ((float)luck / ((float)luck + CRIT_LUCK_DIVISOR)) * 100.0f;
    return (rand() % 100) < (int)crit_chance;
}

/** Add this many movement-speed units per agility point. */
#define AGI_SPEED_FACTOR 0.5f

/** Configure intelligence-based cooldown reduction and its fractional cap. */
#define INT_CD_DIVISOR  200.0f
#define INT_CD_CAP      0.40f

/** Calculate the cooldown multiplier derived from intelligence.
 *
 * @return A multiplier no lower than one minus INT_CD_CAP.
 */
static inline float combat_int_cooldown_mult(int intelligence) {
    float reduction = (float)intelligence / INT_CD_DIVISOR;
    if (reduction > INT_CD_CAP) reduction = INT_CD_CAP;
    return 1.0f - reduction;
}

/** Convert one regeneration point into one percent bonus healing. */
#define REG_HEAL_DIVISOR 100.0f

/** Calculate the healing multiplier derived from regeneration.
 *
 * @return At least 1.0, including for nonpositive regeneration.
 */
static inline float combat_reg_heal_mult(int reg) {
    if (reg <= 0) return 1.0f;
    return 1.0f + ((float)reg / REG_HEAL_DIVISOR);
}

/** Configure base mana regeneration and the contribution per wisdom point. */
#define MANA_REGEN_BASE 1.0f
#define MANA_REGEN_PER_WIS 0.15f

/** Calculate mana regenerated per second from wisdom.
 *
 * @return The mana regeneration rate per second.
 */
static inline float combat_mana_regen_rate(int wisdom) {
    return MANA_REGEN_BASE + ((float)wisdom * MANA_REGEN_PER_WIS);
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