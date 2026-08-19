/**
 * @file
 * Turn a race's data-defined stat curve into the derived numbers combat reads.
 *
 * The four compiled class profiles this file used to hold are gone; races.json owns
 * that data now, and progression.json owns the experience table. Everything that
 * remains is arithmetic, which is why adding a race touches no code here.
 */

#include "class_stats.h"
#include "progression.h"

#include <stdio.h>
#include <string.h>

/**
 * Load the race registry and the progression tunables.
 *
 * @return 1 when at least one race loaded, or 0 otherwise.
 */
int class_stats_init(const char* races_path, const char* progression_path) {
    progression_init(progression_path);

    int races = races_path ? race_registry_init(races_path) : 0;
    if (races == 0) {
        fprintf(stderr, "[CLASS_STATS] No races loaded — character creation will be refused\n");
        return 0;
    }

    printf("[CLASS_STATS] %d races, max level %d\n", races, progression_max_level());
    return 1;
}

/** Release the loaded registry. */
void class_stats_cleanup(void) {
    race_registry_cleanup();
}

/**
 * Return the maximum health implied by a Vitality score.
 */
int class_stats_health_for_vitality(int vitality) {
    const ProgressionConfig* c = progression_config();
    if (vitality < 0) vitality = 0;

    int health = c->base_health + (int)(vitality * c->health_per_vitality);
    return health > 1 ? health : 1;
}

/**
 * Return the pool size implied by a resource type and its governing stat's score.
 *
 * Human Form has no pool at all, so RESOURCE_NONE sizes to zero rather than to the
 * base value — the HUD hides the bar on exactly that condition.
 */
int class_stats_resource_for_stat(ResourceType type, int stat_value) {
    const ProgressionConfig* c = progression_config();
    if (stat_value < 0) stat_value = 0;

    double per_point;
    switch (type) {
        case RESOURCE_MANA:    per_point = c->mana_per_focus;                 break;
        case RESOURCE_STAMINA: per_point = c->stamina_per_stamina_capacity;   break;
        case RESOURCE_RAGE:    per_point = c->rage_per_endurance;             break;
        default:               return 0;
    }

    int pool = c->base_resource + (int)(stat_value * per_point);
    return pool > 0 ? pool : 0;
}

/**
 * Compute the derived statistics for a race at a level.
 *
 * Resource follows the default spec's role, never the race, which is why nothing
 * in races.json configures a pool.
 *
 * @return 1 on success, or 0 for an unknown race or NULL output.
 */
int class_stats_compute(uint32_t race_id, int level, DerivedStats* out) {
    if (!out) return 0;

    const RaceDef* race = race_get(race_id);
    if (!race) return 0;

    int cap = progression_max_level();
    if (level < 1)   level = 1;
    if (level > cap) level = cap;

    memset(out, 0, sizeof(*out));

    for (int stat = 0; stat < STAT_COUNT; stat++) {
        out->stats[stat] = race_stat_at_level(race, (StatId)stat, level);
    }

    const RaceSpec* spec = race_default_spec(race);
    out->resource_type = (uint8_t)(spec ? role_resource(spec->role) : RESOURCE_NONE);

    out->max_health = class_stats_health_for_vitality(out->stats[STAT_VITALITY]);
    out->max_resource = class_stats_resource_for_stat(
        (ResourceType)out->resource_type,
        spec ? out->stats[role_resource_stat(spec->role)] : 0);

    const ProgressionConfig* c = progression_config();
    out->move_speed = race->base_move_speed +
                      (float)(out->stats[STAT_DEXTERITY] * c->move_speed_per_dexterity);

    return 1;
}

/** Return the cumulative experience required to have reached a level. */
uint64_t class_stats_xp_for_level(int level) {
    return progression_xp_for_level(level);
}

/** Return the experience earned within the previous level to reach this one. */
uint64_t class_stats_xp_step(int level) {
    return progression_xp_step(level);
}

/**
 * Advance a level through every experience threshold already reached.
 *
 * @return The resulting level, capped at the configured maximum.
 */
int class_stats_check_level(int current_level, uint64_t current_xp) {
    return progression_check_level(current_level, current_xp);
}

/** Return the level cap in force. */
int class_stats_max_level(void) {
    return progression_max_level();
}
