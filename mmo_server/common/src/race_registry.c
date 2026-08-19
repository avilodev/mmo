/**
 * @file
 * Load races.json into the race registry and answer questions about it.
 *
 * This file replaces a compiled table of four class structs. Nothing here knows how
 * many races exist, what they are called, or what they are good at — all of it is
 * read at startup, so adding a race is a data change and adding a stat is one enum
 * entry plus one key per race.
 */

#include "race_registry.h"
#include "json_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** Map StatId to its JSON key. Indexed by StatId; keep in enum order. */
static const char* const k_stat_keys[STAT_COUNT] = {
    "strength",
    "dexterity",
    "vitality",
    "intelligence",
    "focus",
    "endurance",
    "ferocity",
    "stamina_capacity",
    "precision",
    "ferality",
    "armor",
};

static RaceDef* g_races[MAX_RACES + 1];   /**< Indexed by race id; slot 0 unused. */
static int      g_order[MAX_RACES];       /**< Race ids in load order. */
static int      g_count = 0;

/**
 * Copy a JSON string into a fixed-width field, always terminating.
 */
static void copy_field(char* dst, size_t cap, const JsonValue* obj, const char* key,
                       const char* fallback) {
    const char* src = json_get_string(obj, key, fallback);
    if (!src) src = "";
    size_t len = strlen(src);
    if (len >= cap) len = cap - 1;
    memcpy(dst, src, len);
    dst[len] = '\0';
}

/**
 * Resolve a passive modifier name to its enum value.
 *
 * @return The kind, or -1 when the name is unrecognised.
 */
static int parse_passive_kind(const char* name) {
    if (!name) return -1;
    if (strcmp(name, "damage_taken") == 0) return PASSIVE_MOD_DAMAGE_TAKEN;
    if (strcmp(name, "damage_dealt") == 0) return PASSIVE_MOD_DAMAGE_DEALT;
    if (strcmp(name, "armor") == 0)        return PASSIVE_MOD_ARMOR;
    if (strcmp(name, "move_speed") == 0)   return PASSIVE_MOD_MOVE_SPEED;
    return -1;
}

/**
 * Read a passive's mechanical modifiers.
 *
 * An unrecognised kind is reported rather than dropped silently: a typo would
 * otherwise present as a race whose passive quietly does nothing.
 */
static void parse_passive_modifiers(const JsonValue* obj, RacePassive* passive,
                                    const char* race_key) {
    const JsonValue* arr = json_get(obj, "modifiers");
    int listed = json_count(arr);

    if (listed > MAX_PASSIVE_MODIFIERS) {
        fprintf(stderr, "[RACES] %s passive: %d modifiers exceeds the %d supported; "
                        "the extras are ignored\n", race_key, listed, MAX_PASSIVE_MODIFIERS);
        listed = MAX_PASSIVE_MODIFIERS;
    }

    for (int i = 0; i < listed; i++) {
        const JsonValue* src = json_at(arr, i);
        const char* kind_name = json_get_string(src, "kind", NULL);

        int kind = parse_passive_kind(kind_name);
        if (kind < 0) {
            fprintf(stderr, "[RACES] %s passive: unknown modifier \"%s\" ignored\n",
                    race_key, kind_name ? kind_name : "(missing)");
            continue;
        }

        PassiveModifier* mod = &passive->modifiers[passive->modifier_count++];
        mod->kind       = (PassiveModifierKind)kind;
        mod->value      = json_get_number(src, "value", 0.0);
        mod->per_ally   = json_get_number(src, "per_ally", 0.0);
        mod->max_allies = json_get_int(src, "max_allies", 0);
    }
}

/**
 * Resolve a role name to its enum value.
 *
 * @return The role, or -1 when the name is unrecognised.
 */
static int parse_role(const char* name) {
    if (!name) return -1;
    if (strcmp(name, "tank")   == 0) return ROLE_TANK;
    if (strcmp(name, "dps")    == 0) return ROLE_DPS;
    if (strcmp(name, "healer") == 0) return ROLE_HEALER;
    return -1;
}

/**
 * Read one stat block into a StatId-indexed array.
 *
 * Unknown keys are reported and ignored rather than silently dropped: a typo in a
 * stat name would otherwise present as a race that is quietly weaker than intended.
 *
 * @param out  Receives one entry per stat; missing stats are left at zero.
 */
static void parse_stat_block(const JsonValue* block, int* out, const char* race_key,
                             const char* which) {
    memset(out, 0, sizeof(int) * STAT_COUNT);
    if (!block) return;

    int members = json_member_count(block);
    for (int i = 0; i < members; i++) {
        const char* key = json_key_at(block, i);
        if (!key || key[0] == '_') continue;   /* leading underscore marks a comment */

        int stat = stat_from_key(key);
        if (stat < 0) {
            fprintf(stderr, "[RACES] %s.%s: unknown stat \"%s\" ignored\n",
                    race_key, which, key);
            continue;
        }
        out[stat] = json_as_int(json_member_at(block, i), 0);
    }
}

/**
 * Read one spec, including the ability keys that fill its hotbar.
 *
 * @return 1 on success, or 0 when the spec names no valid role.
 */
static int parse_spec(const JsonValue* obj, RaceSpec* spec, const char* race_key) {
    memset(spec, 0, sizeof(*spec));

    copy_field(spec->id, sizeof(spec->id), obj, "id", "");

    int role = parse_role(json_get_string(obj, "role", NULL));
    if (role < 0) {
        fprintf(stderr, "[RACES] %s spec \"%s\": missing or unknown role\n",
                race_key, spec->id);
        return 0;
    }
    spec->role         = (CombatRole)role;
    spec->is_default   = (uint8_t)json_get_bool(obj, "default", 0);
    spec->unlock_level = (uint8_t)json_get_int(obj, "unlock_level", 1);

    const JsonValue* abilities = json_get(obj, "abilities");
    int listed = json_count(abilities);
    if (listed > MAX_ABILITY_SLOTS) {
        fprintf(stderr, "[RACES] %s spec \"%s\": %d abilities exceeds the %d hotbar slots; "
                        "the extras are ignored\n",
                race_key, spec->id, listed, MAX_ABILITY_SLOTS);
        listed = MAX_ABILITY_SLOTS;
    }
    for (int i = 0; i < listed; i++) {
        const char* key = json_as_string(json_at(abilities, i), NULL);
        if (!key) continue;
        size_t len = strlen(key);
        if (len >= MAX_RACE_KEY) len = MAX_RACE_KEY - 1;
        memcpy(spec->ability_keys[spec->ability_count], key, len);
        spec->ability_keys[spec->ability_count][len] = '\0';
        spec->ability_count++;
    }
    return 1;
}

/**
 * Read one race and install it in the registry.
 *
 * @return 1 when the race was installed, or 0 when it was rejected.
 */
static int parse_race(const JsonValue* obj) {
    int id = json_get_int(obj, "id", 0);
    const char* key = json_get_string(obj, "key", NULL);

    if (id < 1 || id > MAX_RACES) {
        fprintf(stderr, "[RACES] race \"%s\": id %d is outside 1..%d\n",
                key ? key : "?", id, MAX_RACES);
        return 0;
    }
    if (!key || !*key) {
        fprintf(stderr, "[RACES] race id %d: missing key\n", id);
        return 0;
    }
    if (g_races[id]) {
        fprintf(stderr, "[RACES] race \"%s\": id %d already taken by \"%s\"\n",
                key, id, g_races[id]->key);
        return 0;
    }

    RaceDef* race = calloc(1, sizeof(RaceDef));
    if (!race) return 0;

    race->id = (uint32_t)id;
    copy_field(race->key,   sizeof(race->key),   obj, "key",   "");
    copy_field(race->name,  sizeof(race->name),  obj, "name",  race->key);
    copy_field(race->latin, sizeof(race->latin), obj, "latin", "");
    race->playable = (uint8_t)json_get_bool(obj, "playable", 0);

    const JsonValue* passive = json_get(obj, "passive");
    copy_field(race->passive.key,         sizeof(race->passive.key),         passive, "key", "");
    copy_field(race->passive.name,        sizeof(race->passive.name),        passive, "name", "");
    copy_field(race->passive.description, sizeof(race->passive.description), passive, "description", "");
    parse_passive_modifiers(passive, &race->passive, race->key);

    const JsonValue* specs = json_get(obj, "specs");
    int listed = json_count(specs);
    if (listed > MAX_SPECS_PER_RACE) {
        fprintf(stderr, "[RACES] %s: %d specs exceeds the %d supported; the extras are ignored\n",
                race->key, listed, MAX_SPECS_PER_RACE);
        listed = MAX_SPECS_PER_RACE;
    }
    for (int i = 0; i < listed; i++) {
        if (parse_spec(json_at(specs, i), &race->specs[race->spec_count], race->key)) {
            race->spec_count++;
        }
    }

    /* A race with no default falls back to its first spec, so a data file that
     * forgets the flag still produces a playable character rather than an empty bar. */
    race->default_spec = 0;
    for (int i = 0; i < race->spec_count; i++) {
        if (race->specs[i].is_default) { race->default_spec = (uint8_t)i; break; }
    }

    const JsonValue* stats = json_get(obj, "stats");
    parse_stat_block(json_get(stats, "base"),      race->base_stats,      race->key, "base");
    parse_stat_block(json_get(stats, "per_level"), race->per_level_stats, race->key, "per_level");

    race->base_move_speed = (float)json_get_number(obj, "base_move_speed", 200.0);

    g_races[id] = race;
    g_order[g_count++] = id;
    return 1;
}

/**
 * Load races.json into the registry, replacing any previous contents.
 *
 * @param json_filepath  Path to races.json.
 * @return               The number of races loaded, or 0 on failure.
 */
int race_registry_init(const char* json_filepath) {
    race_registry_cleanup();

    const char* err = NULL;
    JsonValue* root = json_parse_file(json_filepath, &err);
    if (!root) {
        fprintf(stderr, "[RACES] %s: %s\n", json_filepath ? json_filepath : "(no path)",
                err ? err : "unreadable");
        return 0;
    }

    const JsonValue* races = json_get(root, "races");
    int listed = json_count(races);
    for (int i = 0; i < listed && g_count < MAX_RACES; i++) {
        parse_race(json_at(races, i));
    }

    json_free(root);

    int playable = 0;
    for (int i = 0; i < g_count; i++) {
        if (g_races[g_order[i]]->playable) playable++;
    }
    printf("[RACES] Loaded %d races from %s (%d playable)\n", g_count, json_filepath, playable);
    return g_count;
}

/**
 * Release the registry.
 */
void race_registry_cleanup(void) {
    for (int i = 0; i <= MAX_RACES; i++) {
        free(g_races[i]);
        g_races[i] = NULL;
    }
    g_count = 0;
}

/** Return how many races are loaded. */
int race_registry_count(void) {
    return g_count;
}

/**
 * Look up a race by its fused identifier.
 *
 * @return A registry-owned definition, or NULL for an unknown identifier.
 */
const RaceDef* race_get(uint32_t race_id) {
    if (race_id < 1 || race_id > MAX_RACES) return NULL;
    return g_races[race_id];
}

/**
 * Look up a race by its stable machine name.
 *
 * @return A registry-owned definition, or NULL for an unknown key.
 */
const RaceDef* race_get_by_key(const char* key) {
    if (!key) return NULL;
    for (int i = 0; i < g_count; i++) {
        RaceDef* race = g_races[g_order[i]];
        if (strcmp(race->key, key) == 0) return race;
    }
    return NULL;
}

/**
 * Return the race at `index` in load order.
 *
 * @return A registry-owned definition, or NULL when out of range.
 */
const RaceDef* race_at(int index) {
    if (index < 0 || index >= g_count) return NULL;
    return g_races[g_order[index]];
}

/**
 * Report whether an untrusted identifier names a race a character may be created as.
 *
 * @return 1 when the race is loaded and playable, or 0 otherwise.
 */
int race_is_playable(uint32_t race_id) {
    const RaceDef* race = race_get(race_id);
    return race && race->playable ? 1 : 0;
}

/** Return a race's spec by index, or NULL when out of range. */
const RaceSpec* race_spec_at(const RaceDef* race, int index) {
    if (!race || index < 0 || index >= race->spec_count) return NULL;
    return &race->specs[index];
}

/**
 * Return the spec a character of this race uses by default.
 *
 * @return A registry-owned spec, or NULL when the race has none.
 */
const RaceSpec* race_default_spec(const RaceDef* race) {
    if (!race || race->spec_count == 0) return NULL;
    return &race->specs[race->default_spec];
}

/**
 * Return the resource a role spends.
 */
ResourceType role_resource(CombatRole role) {
    switch (role) {
        case ROLE_TANK:   return RESOURCE_RAGE;
        case ROLE_DPS:    return RESOURCE_STAMINA;
        case ROLE_HEALER: return RESOURCE_MANA;
        default:          return RESOURCE_NONE;
    }
}

/**
 * Return the stat that sizes a role's resource pool.
 */
StatId role_resource_stat(CombatRole role) {
    switch (role) {
        case ROLE_TANK:   return STAT_ENDURANCE;
        case ROLE_DPS:    return STAT_STAMINA_CAPACITY;
        case ROLE_HEALER: return STAT_FOCUS;
        default:          return STAT_FOCUS;
    }
}

/**
 * Return the total value of one passive modifier kind for a given ally count.
 *
 * @return The summed value, or zero when the passive has no such modifier.
 */
double race_passive_modifier(const RacePassive* passive, PassiveModifierKind kind,
                             int ally_count) {
    if (!passive) return 0.0;
    if (ally_count < 0) ally_count = 0;

    double total = 0.0;
    for (int i = 0; i < passive->modifier_count; i++) {
        const PassiveModifier* mod = &passive->modifiers[i];
        if (mod->kind != kind) continue;

        total += mod->value;

        int allies = ally_count;
        if (allies > mod->max_allies) allies = mod->max_allies;
        total += mod->per_ally * allies;
    }
    return total;
}

/** Return the lowercase name of a role, for logging and client display. */
const char* role_name(CombatRole role) {
    switch (role) {
        case ROLE_TANK:   return "tank";
        case ROLE_DPS:    return "dps";
        case ROLE_HEALER: return "healer";
        default:          return "unknown";
    }
}

/**
 * Return the canonical JSON key of a stat.
 *
 * @return The key, or NULL for an out-of-range index.
 */
const char* stat_key(StatId stat) {
    if (stat < 0 || stat >= STAT_COUNT) return NULL;
    return k_stat_keys[stat];
}

/**
 * Resolve a stat's JSON key to its index.
 *
 * @return The StatId, or -1 when the key names no stat.
 */
int stat_from_key(const char* key) {
    if (!key) return -1;
    for (int i = 0; i < STAT_COUNT; i++) {
        if (strcmp(k_stat_keys[i], key) == 0) return i;
    }
    return -1;
}

/**
 * Compute a race's attribute at a level, folding in the per-level tenths.
 *
 * @return The attribute in whole points, or 0 for an unknown race or stat.
 */
int race_stat_at_level(const RaceDef* race, StatId stat, int level) {
    if (!race || stat < 0 || stat >= STAT_COUNT) return 0;
    if (level < 1) level = 1;
    return race->base_stats[stat] + (race->per_level_stats[stat] * (level - 1)) / 10;
}
