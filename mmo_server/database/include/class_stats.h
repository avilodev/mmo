#ifndef CLASS_STATS_H
#define CLASS_STATS_H

#include <stdint.h>

/** Bound the supported level and class identifier ranges. */
#define MAX_LEVEL 50
#define NUM_CLASSES 4

/** Hold level-adjusted attributes consumed by player initialization. */
typedef struct {
    int     max_health;
    int     max_mana;
    int     strength;
    int     agility;
    int     intelligence;
    int     wisdom;
    int     defense;
    int     evasion;
    int     vitality;
    int     luck;
    /** Store movement speed in world pixels per second. */
    float   move_speed;
} DerivedStats;

/** Define one class's base attributes and per-level growth rates. */
typedef struct {
    const char* class_name;
    int     base_health;
    int     base_mana;
    int     base_strength;
    int     base_agility;
    int     base_intelligence;
    int     base_wisdom;
    int     base_defense;
    int     base_evasion;
    /** Store movement speed in world pixels per second. */
    float   base_move_speed;
    int     health_per_level;
    int     mana_per_level;
    /** Store attribute growth in tenths of a point per level. */
    int     strength_per_level;     /**< Use x10 fixed-point units. */
    int     agility_per_level;
    int     intelligence_per_level;
    int     wisdom_per_level;
    int     defense_per_level;
    int     evasion_per_level;
    int     base_vitality;
    int     base_luck;
    int     vitality_per_level;
    int     luck_per_level;
} ClassStatProfile;

void class_stats_init(void);
const ClassStatProfile* class_stats_get_profile(uint8_t class_id);
int class_stats_compute(uint8_t class_id, int level, DerivedStats* out);
uint64_t class_stats_xp_for_level(int level);
int class_stats_check_level(int current_level, uint64_t current_xp);

#endif