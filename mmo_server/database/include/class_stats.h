#ifndef CLASS_STATS_H
#define CLASS_STATS_H

/** @file Derive a character's level-dependent statistics from data.
 *
 * This module used to hold four hardcoded class profiles and an XP formula. Both
 * are now data: races.json owns the stat curves, progression.json owns the level
 * cap and experience table. What is left here is the arithmetic that turns those
 * curves into the numbers combat actually reads — max health, the resource pool,
 * and movement speed.
 *
 * Race and class fuse into one identifier under the Blessed model, so the
 * `race_id` these functions take is the same value stored in both the class_id and
 * race_id columns.
 */

#include "protocol.h"
#include "race_registry.h"

#include <stdint.h>

/** Hold the level-adjusted values player initialization and combat consume. */
typedef struct {
    int     max_health;
    int     max_resource;
    uint8_t resource_type;         /**< ResourceType, derived from the race's default spec. */
    /** Hold the eleven attributes before gear and buffs, indexed by StatId. */
    int     stats[STAT_COUNT];
    /** Store movement speed in world pixels per second. */
    float   move_speed;
} DerivedStats;

/** Load races.json and progression.json.
 *
 * Both paths may be NULL, in which case the shipped defaults are used and no races
 * are loaded — the server will then refuse every character, which is the correct
 * failure for a missing registry.
 *
 * @return 1 when at least one race loaded, or 0 otherwise.
 */
int class_stats_init(const char* races_path, const char* progression_path);

/** Release the loaded registry. */
void class_stats_cleanup(void);

/** Compute the derived statistics for a race at a level.
 *
 * @param race_id  Fused race/class identifier.
 * @param level    Requested level, clamped to 1 through the configured cap.
 * @param out      Receives the derived statistics; may not be NULL.
 * @return         1 on success, or 0 for an unknown race or NULL output.
 */
int class_stats_compute(uint32_t race_id, int level, DerivedStats* out);

/** Return the maximum health implied by a Vitality score. */
int class_stats_health_for_vitality(int vitality);

/** Return the pool size implied by a resource type and its governing stat's score. */
int class_stats_resource_for_stat(ResourceType type, int stat_value);

/** Return the cumulative experience required to have reached a level. */
uint64_t class_stats_xp_for_level(int level);

/** Return the experience earned within the previous level to reach this one. */
uint64_t class_stats_xp_step(int level);

/** Advance a level through every experience threshold already reached.
 *
 * @return The resulting level, capped at the configured maximum.
 */
int class_stats_check_level(int current_level, uint64_t current_xp);

/** Return the level cap in force. */
int class_stats_max_level(void);

#endif // CLASS_STATS_H
