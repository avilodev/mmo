/**
 * @file
 * Resolve damage from stacked percentage modifiers and flat armor.
 *
 * The whole point of this file is that it is boring: sums, one clamp, one multiply
 * each way, one subtraction. Anything that wants to change how much damage a hit
 * does adds to an accumulator rather than wrapping the calculation.
 */

#include "damage_model.h"
#include "progression.h"

#include <math.h>

/** Clear every accumulator. Tolerates NULL. */
void damage_mods_reset(DamageModifiers* mods) {
    if (!mods) return;
    mods->damage_dealt_pct = 0.0;
    mods->damage_taken_pct = 0.0;
    mods->armor_flat       = 0;
}

/** Add an outgoing damage bonus, as a fraction. */
void damage_mods_add_dealt(DamageModifiers* mods, double pct) {
    if (mods) mods->damage_dealt_pct += pct;
}

/** Add an incoming damage reduction, as a fraction. */
void damage_mods_add_taken(DamageModifiers* mods, double pct) {
    if (mods) mods->damage_taken_pct += pct;
}

/** Add flat armor. */
void damage_mods_add_armor(DamageModifiers* mods, int flat) {
    if (mods) mods->armor_flat += flat;
}

/** Merge one set of accumulators into another, summing each in percentage space. */
void damage_mods_merge(DamageModifiers* into, const DamageModifiers* from) {
    if (!into || !from) return;
    into->damage_dealt_pct += from->damage_dealt_pct;
    into->damage_taken_pct += from->damage_taken_pct;
    into->armor_flat       += from->armor_flat;
}

/**
 * Return the damage-taken reduction actually applied, after the configured clamp.
 *
 * The clamp only ever bites on the reduction side. A negative sum is a stacked
 * vulnerability, which is legitimate and left alone.
 */
double damage_mods_effective_reduction(const DamageModifiers* defender) {
    if (!defender) return 0.0;

    double reduction = defender->damage_taken_pct;
    double cap = progression_config()->damage_taken_reduction_cap;
    if (reduction > cap) reduction = cap;
    return reduction;
}

/**
 * Resolve a final damage figure.
 *
 * @return The damage to apply, never negative, and never below one for a hit that
 *         started with positive damage.
 */
int damage_resolve(int base, const DamageModifiers* attacker, const DamageModifiers* defender) {
    if (base <= 0) return 0;

    double dealt = attacker ? attacker->damage_dealt_pct : 0.0;

    /* A stacked penalty can legitimately zero the outgoing damage, but it must not
     * pass through zero and start healing the target. */
    double outgoing = 1.0 + dealt;
    if (outgoing < 0.0) outgoing = 0.0;

    double amount = (double)base * outgoing;
    amount *= (1.0 - damage_mods_effective_reduction(defender));

    if (defender) amount -= (double)defender->armor_flat;

    /* An attack that connects always does something. Armor mitigates; it never makes
     * a target unhittable, which is what a zero floor here would allow. */
    if (amount < 1.0) return 1;

    /* Round rather than truncate. Summing the same modifiers in a different order
     * produces doubles that differ in the last bit, and truncation turns that into a
     * visibly different damage number — which would quietly break the additive rule
     * this whole file exists to guarantee. Rounding also makes the designed figures
     * exact: 50 x 0.9 is 45, not 44. */
    return (int)floor(amount + 0.5);
}

/**
 * Resolve a final healing figure from the healer's outgoing bonus alone.
 *
 * @return The healing to apply, never negative.
 */
int healing_resolve(int base, const DamageModifiers* healer) {
    if (base <= 0) return 0;

    double outgoing = 1.0 + (healer ? healer->damage_dealt_pct : 0.0);
    if (outgoing < 0.0) outgoing = 0.0;

    double amount = (double)base * outgoing;
    if (amount < 1.0) return amount > 0.0 ? 1 : 0;
    return (int)floor(amount + 0.5);
}

/**
 * Return the healing implied by a percentage of a maximum pool.
 *
 * @return At least one point whenever `percent` and `max_value` are positive.
 */
int percent_of_max(int max_value, double percent) {
    if (max_value <= 0 || percent <= 0.0) return 0;

    double amount = (double)max_value * percent;
    if (amount < 1.0) return 1;
    return (int)floor(amount + 0.5);
}
