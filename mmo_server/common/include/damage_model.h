#ifndef DAMAGE_MODEL_H
#define DAMAGE_MODEL_H

/** @file Resolve a damage figure from stacked percentage modifiers and flat armor.
 *
 * Percentage modifiers of the same kind sum in percentage space and apply once, so
 * two sources of -10% give -20%, never -19%. That makes stacking order-independent,
 * explainable in a tooltip, and cheap: the resolver collects one number per
 * accumulator instead of folding multipliers in an order that would silently matter.
 *
 *     final = base
 *           x (1 + sum of damage_dealt_pct)      attacker's buffs
 *           x (1 - sum of damage_taken_pct)      defender's reducers
 *           - sum of armor                       defender's flat mitigation
 *
 * Additive reduction reaches immunity once enough sources stack, so the summed
 * damage-taken reduction is clamped at a tunable ceiling from progression.json.
 */

/** Accumulate one combatant's modifiers. Zeroed means "no modifiers", not "no effect". */
typedef struct {
    /** Sum of outgoing damage bonuses, as a fraction: 0.10 is +10%. */
    double damage_dealt_pct;
    /** Sum of incoming damage reductions, as a fraction: 0.10 is 10% less taken. */
    double damage_taken_pct;
    /** Sum of flat damage subtracted after the percentages. */
    int    armor_flat;
} DamageModifiers;

/** Clear every accumulator. Tolerates NULL. */
void damage_mods_reset(DamageModifiers* mods);

/** Add an outgoing damage bonus, as a fraction. Negative values are penalties. */
void damage_mods_add_dealt(DamageModifiers* mods, double pct);

/** Add an incoming damage reduction, as a fraction. Negative values are vulnerabilities. */
void damage_mods_add_taken(DamageModifiers* mods, double pct);

/** Add flat armor. */
void damage_mods_add_armor(DamageModifiers* mods, int flat);

/** Merge one set of accumulators into another, summing each in percentage space. */
void damage_mods_merge(DamageModifiers* into, const DamageModifiers* from);

/** Return the damage-taken reduction actually applied, after the configured clamp.
 *
 * Exposed so a tooltip can show the effective number rather than the raw sum.
 */
double damage_mods_effective_reduction(const DamageModifiers* defender);

/** Resolve a final damage figure.
 *
 * Either side may be NULL, meaning that combatant has no modifiers.
 *
 * @param base      Damage before any modifier; zero or less resolves to zero.
 * @param attacker  Supplies damage_dealt_pct; its other fields are ignored.
 * @param defender  Supplies damage_taken_pct and armor_flat; its other fields are ignored.
 * @return          The damage to apply, never negative, and never below one for a
 *                  hit that started with positive damage — an attack that connects
 *                  always does something.
 */
int damage_resolve(int base, const DamageModifiers* attacker, const DamageModifiers* defender);

/** Resolve a final healing figure from the attacker's outgoing bonus alone.
 *
 * Healing is never reduced by armor or by the target's damage-taken accumulator.
 *
 * @return The healing to apply, never negative.
 */
int healing_resolve(int base, const DamageModifiers* healer);

/** Return the healing implied by a percentage of a maximum pool.
 *
 * Percent-of-max healing is what Bite, Hibernate and Second Wind all express, and
 * it is the one shape a flat integer cannot represent.
 *
 * @param percent  A fraction: 0.05 is five percent.
 * @return         At least one point whenever `percent` and `max_value` are positive.
 */
int percent_of_max(int max_value, double percent);

#endif // DAMAGE_MODEL_H
