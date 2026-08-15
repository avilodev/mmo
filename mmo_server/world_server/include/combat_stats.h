// ============================================================================
// combat_stats.h — Shared formulas for stat-based combat calculations
//
// All damage/defense/evasion formulas live here so combat.c and
// ability_handler.c use the same math.
// ============================================================================

#ifndef COMBAT_STATS_H
#define COMBAT_STATS_H

#include <stdlib.h>
#include <math.h>

// ---------------------------------------------------------------------------
// Damage multiplier from a specific stat (percentage-based)
//
// damage_stat is a StatType int value (defined in ability_def.h):
//   STAT_STRENGTH=4, STAT_AGILITY=5, STAT_INTELLIGENCE=6, STAT_WISDOM=7
//
// Formula: 1.0 + (stat_value / STAT_DAMAGE_DIVISOR)
//
// STAT_NONE (0) or any unlisted value returns 1.0 — no scaling.
// This is the per-ability scaling path; set "damageStat" in abilities.json.
//
// Examples with divisor 100:
//   stat=50  -> 1.50x (50% bonus)
//   stat=100 -> 2.00x (100% bonus)
//   stat=200 -> 3.00x (200% bonus)
// ---------------------------------------------------------------------------
#define STAT_DAMAGE_DIVISOR 100.0f

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

// ---------------------------------------------------------------------------
// Defense damage reduction (diminishing returns)
//
// Formula: reduction_multiplier = 100 / (100 + defense)
// So final_damage = raw_damage * 100 / (100 + defense)
//
// Examples:
//   defense=0   -> 100% damage taken
//   defense=50  -> 67% damage taken
//   defense=100 -> 50% damage taken
//   defense=200 -> 33% damage taken
// ---------------------------------------------------------------------------
static inline int combat_apply_defense(int raw_damage, int defense) {
    if (defense <= 0) return raw_damage;
    int reduced = (raw_damage * 100) / (100 + defense);
    return (reduced < 1) ? 1 : reduced;  // Always deal at least 1 damage
}

// ---------------------------------------------------------------------------
// Evasion / dodge check
//
// Dodge chance = evasion / (evasion + 100) * 100%
// Capped at 60% to prevent untouchable builds.
//
// Examples:
//   evasion=0   -> 0% dodge
//   evasion=20  -> 17% dodge
//   evasion=50  -> 33% dodge
//   evasion=100 -> 50% dodge
//   evasion=200 -> 60% dodge (capped)
//
// Returns 1 if the attack is dodged, 0 if it hits.
// ---------------------------------------------------------------------------
#define EVASION_CAP_PERCENT 60

static inline int combat_check_evasion(int evasion) {
    if (evasion <= 0) return 0;
    int dodge_chance = (evasion * 100) / (evasion + 100);
    if (dodge_chance > EVASION_CAP_PERCENT) dodge_chance = EVASION_CAP_PERCENT;
    int roll = rand() % 100;
    return (roll < dodge_chance) ? 1 : 0;
}

// ---------------------------------------------------------------------------
// Critical hit check — driven by the luck stat
//
// Formula: crit_chance% = luck / (luck + CRIT_LUCK_DIVISOR) * 100
//
// Examples with divisor 150:
//   luck=50  -> 25% crit chance
//   luck=100 -> 40% crit chance
//   luck=150 -> 50% crit chance
//   luck=300 -> 67% crit chance (no hard cap — luck can always be invested in)
//
// On a crit, damage/healing is multiplied by CRIT_DAMAGE_MULTIPLIER.
// Raise CRIT_LUCK_DIVISOR to make crits harder to reach.
// Raise CRIT_DAMAGE_MULTIPLIER to make crits feel more rewarding.
// ---------------------------------------------------------------------------
#define CRIT_LUCK_DIVISOR    150.0f
#define CRIT_DAMAGE_MULTIPLIER 1.75f

static inline int combat_check_crit(int luck) {
    if (luck <= 0) return 0;
    float crit_chance = ((float)luck / ((float)luck + CRIT_LUCK_DIVISOR)) * 100.0f;
    return (rand() % 100) < (int)crit_chance;
}

// ---------------------------------------------------------------------------
// AGI contribution to move_speed
//
// Formula: base_speed + (agility * AGI_SPEED_FACTOR)
//
// Examples with factor 0.5:
//   agi=20  -> +10 speed
//   agi=50  -> +25 speed
//   agi=100 -> +50 speed
// ---------------------------------------------------------------------------
#define AGI_SPEED_FACTOR 0.5f

// ---------------------------------------------------------------------------
// INT cooldown reduction — applies to ALL classes
//
// Formula: 1.0 - min(int / INT_CD_DIVISOR, INT_CD_CAP)
// Returns a multiplier (e.g. 0.80 = 20% CDR)
//
// Examples with divisor 200, cap 0.40:
//   int=40  -> 20% CDR (0.80x)
//   int=80  -> 40% CDR (0.60x, capped)
//   int=200 -> 40% CDR (capped)
// ---------------------------------------------------------------------------
#define INT_CD_DIVISOR  200.0f
#define INT_CD_CAP      0.40f

static inline float combat_int_cooldown_mult(int intelligence) {
    float reduction = (float)intelligence / INT_CD_DIVISOR;
    if (reduction > INT_CD_CAP) reduction = INT_CD_CAP;
    return 1.0f - reduction;
}

// ---------------------------------------------------------------------------
// REG healing rate multiplier
//
// Formula: 1.0 + (reg / REG_HEAL_DIVISOR)
// Every point of REG from any source contributes an equal % bonus to heals.
//
// Examples with divisor 100:
//   reg=50  -> 1.50x heals
//   reg=100 -> 2.00x heals
// ---------------------------------------------------------------------------
#define REG_HEAL_DIVISOR 100.0f

static inline float combat_reg_heal_mult(int reg) {
    if (reg <= 0) return 1.0f;
    return 1.0f + ((float)reg / REG_HEAL_DIVISOR);
}

// ---------------------------------------------------------------------------
// Mana regen per second, scaled by wisdom
//
// Formula: base_regen + (wisdom * 0.15)
// Base regen = 1.0 mana/sec for everyone
// A Landweaver with 50 WIS regens 1 + 7.5 = 8.5/sec
// A Gladiator with 5 WIS regens 1 + 0.75 = 1.75/sec
// ---------------------------------------------------------------------------
#define MANA_REGEN_BASE 1.0f
#define MANA_REGEN_PER_WIS 0.15f

static inline float combat_mana_regen_rate(int wisdom) {
    return MANA_REGEN_BASE + ((float)wisdom * MANA_REGEN_PER_WIS);
}

// ---------------------------------------------------------------------------
// HP regen per second (out-of-combat only)
//
// Formula: 0.5 + (max_health * 0.005)
// A Gladiator L10 with ~375 HP regens ~2.4 HP/sec OOC
// A Spirit L10 with ~170 HP regens ~1.35 HP/sec OOC
//
// "Out of combat" = no damage taken or dealt in the last N seconds
// ---------------------------------------------------------------------------
#define HP_REGEN_BASE 0.5f
#define HP_REGEN_HP_RATIO 0.005f
#define OOC_THRESHOLD_SECONDS 5.0

static inline float combat_hp_regen_rate(int max_health) {
    return HP_REGEN_BASE + ((float)max_health * HP_REGEN_HP_RATIO);
}

#endif // COMBAT_STATS_H