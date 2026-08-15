#include "class_stats.h"
#include <stdio.h>
#include <string.h>

static ClassStatProfile g_class_stats[NUM_CLASSES + 1];
static int g_initialized = 0;
static uint64_t g_xp_table[MAX_LEVEL + 1];

void class_stats_init(void) {
    if (g_initialized) return;
    memset(g_class_stats, 0, sizeof(g_class_stats));

    g_class_stats[0].class_name = "None";

    // [1] Gladiator — frontline tank/bruiser
    g_class_stats[1] = (ClassStatProfile){
        .class_name = "Gladiator",
        .base_health = 150, .base_mana = 10,
        .base_strength = 12, .base_agility = 5,
        .base_intelligence = 2, .base_wisdom = 2,
        .base_defense = 10, .base_evasion = 2,
        .base_move_speed = 200.0f,
        .health_per_level = 25, .mana_per_level = 2,
        .strength_per_level = 30, .agility_per_level = 10,
        .intelligence_per_level = 5, .wisdom_per_level = 5,
        .defense_per_level = 25, .evasion_per_level = 5,
        .base_vitality = 8, .base_luck = 2,
        .vitality_per_level = 20, .luck_per_level = 5,
    };

    // [2] Ninja — agile burst / evasion
    g_class_stats[2] = (ClassStatProfile){
        .class_name = "Ninja",
        .base_health = 100, .base_mana = 300,
        .base_strength = 6, .base_agility = 14,
        .base_intelligence = 4, .base_wisdom = 3,
        .base_defense = 4, .base_evasion = 12,
        .base_move_speed = 230.0f,
        .health_per_level = 15, .mana_per_level = 5,
        .strength_per_level = 10, .agility_per_level = 30,
        .intelligence_per_level = 10, .wisdom_per_level = 5,
        .defense_per_level = 10, .evasion_per_level = 30,
        .base_vitality = 4, .base_luck = 8,
        .vitality_per_level = 10, .luck_per_level = 20,
    };

    // [3] Landweaver — earth mage, control/AoE
    g_class_stats[3] = (ClassStatProfile){
        .class_name = "Landweaver",
        .base_health = 80, .base_mana = 600,
        .base_strength = 3, .base_agility = 3,
        .base_intelligence = 14, .base_wisdom = 8,
        .base_defense = 6, .base_evasion = 2,
        .base_move_speed = 185.0f,
        .health_per_level = 10, .mana_per_level = 12,
        .strength_per_level = 5, .agility_per_level = 5,
        .intelligence_per_level = 30, .wisdom_per_level = 20,
        .defense_per_level = 15, .evasion_per_level = 5,
        .base_vitality = 5, .base_luck = 5,
        .vitality_per_level = 12, .luck_per_level = 12,
    };

    // [4] Spirit — mystic support/hybrid
    g_class_stats[4] = (ClassStatProfile){
        .class_name = "Spirit",
        .base_health = 80, .base_mana = 800,
        .base_strength = 3, .base_agility = 5,
        .base_intelligence = 8, .base_wisdom = 14,
        .base_defense = 3, .base_evasion = 8,
        .base_move_speed = 200.0f,
        .health_per_level = 10, .mana_per_level = 10,
        .strength_per_level = 5, .agility_per_level = 10,
        .intelligence_per_level = 20, .wisdom_per_level = 30,
        .defense_per_level = 5, .evasion_per_level = 20,
        .base_vitality = 4, .base_luck = 6,
        .vitality_per_level = 10, .luck_per_level = 15,
    };

    // XP table: xp_for_level(n) = 50 * n * (n-1)
    g_xp_table[0] = 0;
    g_xp_table[1] = 0;
    for (int i = 2; i <= MAX_LEVEL; i++) {
        g_xp_table[i] = (uint64_t)(50 * i * (i - 1));
    }

    g_initialized = 1;
    printf("[CLASS_STATS] Initialized %d class profiles, max level %d\n",
           NUM_CLASSES, MAX_LEVEL);
    printf("[CLASS_STATS] XP curve: L2=%lu, L10=%lu, L25=%lu, L50=%lu\n",
           g_xp_table[2], g_xp_table[10], g_xp_table[25], g_xp_table[50]);
}

const ClassStatProfile* class_stats_get_profile(uint8_t class_id) {
    if (class_id < 1 || class_id > NUM_CLASSES) return NULL;
    return &g_class_stats[class_id];
}

int class_stats_compute(uint8_t class_id, int level, DerivedStats* out) {
    if (!out) return 0;
    if (class_id < 1 || class_id > NUM_CLASSES) return 0;
    if (level < 1) level = 1;
    if (level > MAX_LEVEL) level = MAX_LEVEL;

    const ClassStatProfile* p = &g_class_stats[class_id];
    int lvl_bonus = level - 1;

    out->max_health     = p->base_health    + (p->health_per_level * lvl_bonus);
    out->max_mana       = p->base_mana      + (p->mana_per_level   * lvl_bonus);
    out->strength       = p->base_strength  + (p->strength_per_level * lvl_bonus) / 10;
    out->agility        = p->base_agility   + (p->agility_per_level * lvl_bonus) / 10;
    out->intelligence   = p->base_intelligence + (p->intelligence_per_level * lvl_bonus) / 10;
    out->wisdom         = p->base_wisdom    + (p->wisdom_per_level * lvl_bonus) / 10;
    out->defense        = p->base_defense   + (p->defense_per_level * lvl_bonus) / 10;
    out->evasion        = p->base_evasion   + (p->evasion_per_level * lvl_bonus) / 10;
    out->vitality       = p->base_vitality + (p->vitality_per_level * lvl_bonus) / 10;
    out->luck           = p->base_luck     + (p->luck_per_level * lvl_bonus) / 10;
    out->move_speed     = p->base_move_speed;

    return 1;
}

uint64_t class_stats_xp_for_level(int level) {
    if (level < 1) return 0;
    if (level > MAX_LEVEL) return g_xp_table[MAX_LEVEL];
    return g_xp_table[level];
}

int class_stats_check_level(int current_level, uint64_t current_xp) {
    if (current_level >= MAX_LEVEL) return MAX_LEVEL;
    int new_level = current_level;
    while (new_level < MAX_LEVEL && current_xp >= g_xp_table[new_level + 1]) {
        new_level++;
    }
    return new_level;
}