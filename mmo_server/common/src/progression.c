/**
 * @file
 * Load progression tunables and derive the experience table from them.
 *
 * The table is geometric: reaching level 2 costs `xp_base`, and every level after
 * costs `xp_growth` times the one before. Storing the cumulative thresholds lets
 * level checks be a comparison instead of a sum, and keeping the per-level steps
 * alongside them means the XP bar never has to subtract two large numbers.
 */

#include "progression.h"
#include "log.h"
#include "json_util.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/** Hold the shipped values, used only when progression.json cannot be read. */
static const ProgressionConfig k_defaults = {
    .max_level                     = 30,
    .xp_base                       = 100.0,
    .xp_growth                     = 1.5,

    .damage_taken_reduction_cap    = 0.80,
    .form_swap_cooldown            = 1.5,

    .base_health                   = 60,
    .health_per_vitality           = 12.0,
    .base_resource                 = 50,
    .mana_per_focus                = 10.0,
    .stamina_per_stamina_capacity  = 6.0,
    .rage_per_endurance            = 4.0,

    .move_speed_per_dexterity      = 1.5,
    .crit_chance_per_precision     = 0.5,
    .crit_damage_per_ferocity      = 0.02,
    .crit_chance_cap               = 60.0,

    .animal_power_per_ferality     = 0.01,

    .rage_per_damage_dealt         = 0.5,
    .rage_per_damage_taken         = 1.0,
    .rage_decay_per_second         = 3.0,
    .rage_decay_delay              = 5.0,

    .mana_regen_per_focus          = 0.20,
    .stamina_regen_per_capacity    = 0.15,
    .resource_regen_base           = 2.0,
};

static ProgressionConfig g_config;
static uint64_t g_xp_total[PROGRESSION_MAX_LEVEL_CAP + 1];
static uint64_t g_xp_step [PROGRESSION_MAX_LEVEL_CAP + 1];
static int g_loaded = 0;

/**
 * Recompute both experience tables from the current tunables.
 *
 * Steps are rounded to whole points so the number shown on an XP bar is the number
 * actually required. Growth below 1 would make the curve fall away, and a step is
 * never allowed below one point, so the table always stays strictly increasing.
 */
static void rebuild_xp_tables(void) {
    memset(g_xp_total, 0, sizeof(g_xp_total));
    memset(g_xp_step, 0, sizeof(g_xp_step));

    double growth = g_config.xp_growth > 0.0 ? g_config.xp_growth : 1.0;
    double cost   = g_config.xp_base > 0.0 ? g_config.xp_base : 1.0;
    uint64_t total = 0;

    for (int level = 2; level <= g_config.max_level; level++) {
        uint64_t step = (uint64_t)floor(cost + 0.5);
        if (step < 1) step = 1;

        /* Saturate rather than wrap: a mistuned growth factor should produce an
         * unreachable level, not a level that costs nothing. */
        if (total > UINT64_MAX - step) total = UINT64_MAX;
        else                           total += step;

        g_xp_step[level]  = step;
        g_xp_total[level] = total;

        cost *= growth;
    }
}

/**
 * Read one tunable from the config object, keeping the default when absent.
 */
static double read_tunable(const JsonValue* obj, const char* key, double fallback) {
    return json_get_number(obj, key, fallback);
}

/**
 * Load progression.json, falling back to the compiled defaults on any failure.
 *
 * @param json_filepath  Path to progression.json; NULL selects the defaults.
 * @return               1 when the file was loaded, or 0 when the defaults were used.
 */
int progression_init(const char* json_filepath) {
    g_config = k_defaults;
    g_loaded = 1;

    if (!json_filepath) {
        rebuild_xp_tables();
        LOG_INFO("[PROGRESSION] No file given; using built-in defaults (max level %d)",
                 g_config.max_level);
        return 0;
    }

    const char* err = NULL;
    JsonValue* root = json_parse_file(json_filepath, &err);
    if (!root) {
        rebuild_xp_tables();
        LOG_ERROR("[PROGRESSION] %s: %s — using built-in defaults",
                  json_filepath, err ? err : "unreadable");
        return 0;
    }

    g_config.max_level = json_get_int(root, "max_level", k_defaults.max_level);
    if (g_config.max_level < 1) g_config.max_level = 1;
    if (g_config.max_level > PROGRESSION_MAX_LEVEL_CAP) {
        LOG_ERROR("[PROGRESSION] max_level %d exceeds the %d cap; clamping",
                  g_config.max_level, PROGRESSION_MAX_LEVEL_CAP);
        g_config.max_level = PROGRESSION_MAX_LEVEL_CAP;
    }

    const JsonValue* xp = json_get(root, "xp");
    g_config.xp_base   = read_tunable(xp, "base",   k_defaults.xp_base);
    g_config.xp_growth = read_tunable(xp, "growth", k_defaults.xp_growth);

    const JsonValue* t = json_get(root, "tunables");
    #define READ(field) g_config.field = read_tunable(t, #field, k_defaults.field)
    READ(damage_taken_reduction_cap);
    READ(form_swap_cooldown);
    READ(health_per_vitality);
    READ(base_resource);
    READ(mana_per_focus);
    READ(stamina_per_stamina_capacity);
    READ(rage_per_endurance);
    READ(move_speed_per_dexterity);
    READ(crit_chance_per_precision);
    READ(crit_damage_per_ferocity);
    READ(crit_chance_cap);
    READ(animal_power_per_ferality);
    READ(rage_per_damage_dealt);
    READ(rage_per_damage_taken);
    READ(rage_decay_per_second);
    READ(rage_decay_delay);
    READ(mana_regen_per_focus);
    READ(stamina_regen_per_capacity);
    READ(resource_regen_base);
    #undef READ

    g_config.base_health   = json_get_int(t, "base_health",   k_defaults.base_health);
    g_config.base_resource = json_get_int(t, "base_resource", k_defaults.base_resource);

    /* An uncapped reduction lets stacked sources reach immunity, which is a bug
     * class rather than a tuning choice. Refuse the value, do not honour it. */
    if (g_config.damage_taken_reduction_cap < 0.0 || g_config.damage_taken_reduction_cap > 0.99) {
        LOG_ERROR("[PROGRESSION] damage_taken_reduction_cap %.2f out of range; using %.2f",
                  g_config.damage_taken_reduction_cap, k_defaults.damage_taken_reduction_cap);
        g_config.damage_taken_reduction_cap = k_defaults.damage_taken_reduction_cap;
    }

    json_free(root);
    rebuild_xp_tables();

    LOG_INFO("[PROGRESSION] Loaded %s: max level %d, XP base %.0f x%.2f",
             json_filepath, g_config.max_level, g_config.xp_base, g_config.xp_growth);
    LOG_INFO("[PROGRESSION] XP steps: L2=%lu L10=%lu L20=%lu L%d=%lu (cumulative %lu)",
             (unsigned long)progression_xp_step(2),
             (unsigned long)progression_xp_step(10),
             (unsigned long)progression_xp_step(20),
             g_config.max_level,
             (unsigned long)progression_xp_step(g_config.max_level),
             (unsigned long)progression_xp_for_level(g_config.max_level));
    return 1;
}

/** Return the loaded tunables. Never NULL; holds defaults before progression_init(). */
const ProgressionConfig* progression_config(void) {
    if (!g_loaded) {
        g_config = k_defaults;
        rebuild_xp_tables();
        g_loaded = 1;
    }
    return &g_config;
}

/** Return the level cap actually in force. */
int progression_max_level(void) {
    return progression_config()->max_level;
}

/**
 * Return the cumulative experience required to have reached `level`.
 *
 * @return Zero at or below level one, and the cap's threshold above the cap.
 */
uint64_t progression_xp_for_level(int level) {
    int cap = progression_max_level();
    if (level < 2) return 0;
    if (level > cap) return g_xp_total[cap];
    return g_xp_total[level];
}

/**
 * Return the experience earned within level `level - 1` to reach `level`.
 *
 * @return Zero at or below level one and above the cap.
 */
uint64_t progression_xp_step(int level) {
    int cap = progression_max_level();
    if (level < 2 || level > cap) return 0;
    return g_xp_step[level];
}

/**
 * Advance a level through every threshold the given experience total has passed.
 *
 * @return The resulting level, capped at the configured maximum.
 */
int progression_check_level(int current_level, uint64_t current_xp) {
    int cap = progression_max_level();
    if (current_level >= cap) return cap;
    if (current_level < 1) current_level = 1;

    int level = current_level;
    while (level < cap && current_xp >= g_xp_total[level + 1]) level++;
    return level;
}
