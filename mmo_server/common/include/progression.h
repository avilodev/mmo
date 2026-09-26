#ifndef PROGRESSION_H
#define PROGRESSION_H

/** @file Load level, experience, and combat tunables from data.
 *
 * Every number a playtest is likely to move lives here rather than in a header,
 * because `make run` recopies the JSON data files without rebuilding. Retuning damage,
 * cooldowns, XP growth, or stat weights must never require a recompile.
 *
 * The defaults compiled in below are the values progression.json ships with. They
 * exist only so the server still starts if the file is missing or malformed, and
 * they are never the tuning surface.
 */

#include <stdint.h>

/** Bound the level table. The shipped cap is 30; this only sizes the array. */
#define PROGRESSION_MAX_LEVEL_CAP 200

/** Hold every tunable read from progression.json. */
typedef struct {
    int    max_level;             /**< Level cap; clamped to PROGRESSION_MAX_LEVEL_CAP. */
    double xp_base;               /**< Experience to reach level 2. */
    double xp_growth;             /**< Multiplier applied to each successive level's cost. */

    /** Clamp the summed damage-taken reduction so stacked sources cannot reach immunity. */
    double damage_taken_reduction_cap;
    /** Seconds of shared cooldown after a form swap; 0 makes swapping free. */
    double form_swap_cooldown;

    int    base_health;           /**< Health before any Vitality contribution. */
    double health_per_vitality;
    int    base_resource;         /**< Pool size before any resource-stat contribution. */
    double mana_per_focus;
    double stamina_per_stamina_capacity;
    double rage_per_endurance;

    double move_speed_per_dexterity;
    double crit_chance_per_precision;   /**< Percentage points of crit chance per point. */
    double crit_damage_per_ferocity;    /**< Added to the 1.0 base multiplier, per point. */
    double crit_chance_cap;             /**< Maximum crit chance in percentage points. */

    /** Scale Animal Form ability output: multiplier = 1 + ferality * this. */
    double animal_power_per_ferality;

    /** Rage gained per point of damage dealt and taken, and decayed per second. */
    double rage_per_damage_dealt;
    double rage_per_damage_taken;
    double rage_decay_per_second;
    /** Seconds after the last combat event before rage begins decaying. */
    double rage_decay_delay;

    double mana_regen_per_focus;        /**< Points restored per second, per point of Focus. */
    double stamina_regen_per_capacity;  /**< Points restored per second, per point of capacity. */
    double resource_regen_base;         /**< Points restored per second regardless of stats. */
} ProgressionConfig;

/** Load progression.json, falling back to the compiled defaults on any failure.
 *
 * Safe to call repeatedly; each call replaces the loaded table.
 *
 * @param json_filepath  Path to progression.json; NULL selects the defaults.
 * @return               1 when the file was loaded, or 0 when the defaults were used.
 */
int progression_init(const char* json_filepath);

/** Return the loaded tunables. Never NULL; holds defaults before progression_init(). */
const ProgressionConfig* progression_config(void);

/** Return the level cap actually in force. */
int progression_max_level(void);

/** Return the cumulative experience required to have reached `level`.
 *
 * @return Zero at or below level one, and the cap's threshold above the cap.
 */
uint64_t progression_xp_for_level(int level);

/** Return the experience earned within level `level - 1` to reach `level`.
 *
 * This is the number a player sees on an XP bar, and the one the curve is tuned by.
 *
 * @return Zero at or below level one and above the cap.
 */
uint64_t progression_xp_step(int level);

/** Advance a level through every threshold the given experience total has passed.
 *
 * @return The resulting level, capped at the configured maximum.
 */
int progression_check_level(int current_level, uint64_t current_xp);

#endif // PROGRESSION_H
