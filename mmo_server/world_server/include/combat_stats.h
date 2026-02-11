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
// Damage scaling by primary stat
//
// class_id: 1=Gladiator, 2=Ninja, 3=Landweaver, 4=Spirit
// Returns the bonus damage from the player's primary stat.
// Formula: primary_stat * 0.5  (tunable)
// ---------------------------------------------------------------------------
static inline int combat_stat_bonus_damage(int strength, int agility,
                                            int intelligence, int wisdom,
                                            uint8_t class_id) {
    switch (class_id) {
        case 1: return strength / 2;       // Gladiator scales with STR
        case 2: return agility / 2;        // Ninja scales with AGI
        case 3: return intelligence / 2;   // Landweaver scales with INT
        case 4: return wisdom / 2;         // Spirit scales with WIS
        default: return 0;
    }
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