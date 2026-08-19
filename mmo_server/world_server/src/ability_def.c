/**
 * @file
 * Load ability definitions and provide world-server ability lookups.
 *
 * Abilities are keyed objects in abilities.json. Adding one is a single entry plus
 * an icon — the only thing that ever needs code is a genuinely new effect verb, and
 * those live in the StatusEffectType enum rather than here.
 */

#include "ability_def.h"
#include "json_util.h"
#include "race_registry.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static AbilityDef* ability_table[MAX_ABILITIES];   // Indexed by numeric ID
static int abilities_loaded = 0;
static uint16_t next_ability_id = 1;               // Auto-assigned, 1-based

/**
 * Copy a JSON string into a fixed-width field, always terminating.
 */
static void copy_field(char* dst, size_t cap, const JsonValue* obj, const char* key) {
    const char* src = json_get_string(obj, key, "");
    size_t len = strlen(src);
    if (len >= cap) len = cap - 1;
    memcpy(dst, src, len);
    dst[len] = '\0';
}

/** Resolve an area shape name. */
static AbilityAoeShape parse_aoe_shape(const char* str) {
    if (!str) return ABILITY_AOE_NONE;
    if (strcmp(str, "circle") == 0)    return ABILITY_AOE_CIRCLE;
    if (strcmp(str, "sphere") == 0)    return ABILITY_AOE_CIRCLE; // treat sphere as circle in 2D
    if (strcmp(str, "cone") == 0)      return ABILITY_AOE_CONE;
    if (strcmp(str, "rectangle") == 0) return ABILITY_AOE_RECTANGLE;
    return ABILITY_AOE_NONE;
}

/** Resolve a damage school name. */
static AbilityDamageType parse_damage_type(const char* str) {
    if (!str) return ABILITY_DMG_PHYSICAL;
    if (strcmp(str, "physical") == 0) return ABILITY_DMG_PHYSICAL;
    if (strcmp(str, "earth") == 0)    return ABILITY_DMG_EARTH;
    if (strcmp(str, "spirit") == 0)   return ABILITY_DMG_SPIRIT;
    return ABILITY_DMG_PHYSICAL;
}

/** Resolve a targeting name. */
static AbilityTargetType parse_target_type(const char* str) {
    if (!str) return ABILITY_TARGET_ENEMY;
    if (strcmp(str, "ally") == 0)   return ABILITY_TARGET_ALLY;
    if (strcmp(str, "self") == 0)   return ABILITY_TARGET_SELF;
    if (strcmp(str, "ground") == 0) return ABILITY_TARGET_GROUND;
    return ABILITY_TARGET_ENEMY;
}

/**
 * Resolve an effect name.
 *
 * @return The effect type, or EFFECT_NONE when the name is unrecognised.
 */
static StatusEffectType parse_effect_type(const char* str) {
    if (!str) return EFFECT_NONE;
    if (strcmp(str, "dot") == 0)           return EFFECT_DOT;
    if (strcmp(str, "hot") == 0)           return EFFECT_HOT;
    if (strcmp(str, "stun") == 0)          return EFFECT_STUN;
    if (strcmp(str, "slow") == 0)          return EFFECT_SLOW;
    if (strcmp(str, "buff") == 0)          return EFFECT_BUFF;
    if (strcmp(str, "stealth") == 0)       return EFFECT_STEALTH;
    if (strcmp(str, "knockup") == 0)       return EFFECT_KNOCKUP;
    if (strcmp(str, "link") == 0)          return EFFECT_LINK;
    if (strcmp(str, "cleanse") == 0)       return EFFECT_CLEANSE;
    if (strcmp(str, "taunt") == 0)         return EFFECT_TAUNT;
    if (strcmp(str, "resource") == 0)      return EFFECT_RESOURCE;
    if (strcmp(str, "damage_taken") == 0)  return EFFECT_DAMAGE_TAKEN;
    if (strcmp(str, "damage_dealt") == 0)  return EFFECT_DAMAGE_DEALT;
    if (strcmp(str, "root") == 0)          return EFFECT_ROOT;
    if (strcmp(str, "channel") == 0)       return EFFECT_CHANNEL;
    if (strcmp(str, "hot_percent") == 0)   return EFFECT_HOT_PERCENT;
    return EFFECT_NONE;
}

/**
 * Resolve a scaling or buff target.
 *
 * Attribute names come from the race registry, so the eleven stats are spelled the
 * same way in abilities.json and races.json.
 *
 * @return The target, or STAT_TARGET_NONE when the name is unrecognised.
 */
static StatType parse_stat_type(const char* str) {
    if (!str || !*str || strcmp(str, "none") == 0) return STAT_TARGET_NONE;

    int stat = stat_from_key(str);
    if (stat >= 0) return (StatType)stat;

    if (strcmp(str, "move_speed") == 0)    return STAT_TARGET_MOVE_SPEED;
    if (strcmp(str, "weapon_damage") == 0) return STAT_TARGET_WEAPON_DAMAGE;
    return STAT_TARGET_NONE;
}

/** Resolve a spawned-entity name. */
static SpawnEntityType parse_spawn_type(const char* str) {
    if (!str) return SPAWN_NONE;
    if (strcmp(str, "wall") == 0)  return SPAWN_WALL;
    if (strcmp(str, "zone") == 0)  return SPAWN_ZONE;
    if (strcmp(str, "decoy") == 0) return SPAWN_DECOY;
    return SPAWN_NONE;
}

/** Resolve a movement name. */
static AbilityMovementType parse_movement_type(const char* str) {
    if (!str) return MOVEMENT_NONE;
    if (strcmp(str, "teleport") == 0) return MOVEMENT_TELEPORT;
    if (strcmp(str, "dash") == 0)     return MOVEMENT_DASH;
    return MOVEMENT_NONE;
}

/** Resolve a projectile name. */
static AbilityProjectileType parse_projectile_type(const char* str) {
    if (!str) return PROJECTILE_NONE;
    if (strcmp(str, "linear") == 0) return PROJECTILE_LINEAR;
    return PROJECTILE_NONE;
}

/** Resolve a form name; abilities default to Animal Form. */
static uint8_t parse_form(const char* str) {
    if (str && strcmp(str, "human") == 0) return FORM_HUMAN;
    return FORM_ANIMAL;
}

/** Parse an optional area-of-effect component. */
static void parse_aoe(const JsonValue* obj, AbilityAoeDef* aoe) {
    const JsonValue* src = json_get(obj, "aoe");
    if (!src) return;

    aoe->shape  = parse_aoe_shape(json_get_string(src, "shape", NULL));
    aoe->radius = (float)json_get_number(src, "radius", 0.0);
    aoe->angle  = (float)json_get_number(src, "angle", 0.0);
    aoe->width  = (float)json_get_number(src, "width", 0.0);
    aoe->height = (float)json_get_number(src, "height", 0.0);
}

/** Parse an optional spawned-entity component. */
static void parse_spawn(const JsonValue* obj, AbilitySpawnDef* spawn) {
    const JsonValue* src = json_get(obj, "spawnsEntity");
    if (!src) return;

    spawn->type                = parse_spawn_type(json_get_string(src, "type", NULL));
    spawn->duration            = (float)json_get_number(src, "duration", 0.0);
    spawn->has_collision       = (uint8_t)json_get_bool(src, "hasCollision", 0);
    spawn->hp                  = json_get_int(src, "hp", 0);
    spawn->inherits_appearance = (uint8_t)json_get_bool(src, "inheritsAppearance", 0);
}

/** Parse an optional movement component. */
static void parse_movement(const JsonValue* obj, AbilityMovementDef* move) {
    const JsonValue* src = json_get(obj, "movement");
    if (!src) return;

    move->type     = parse_movement_type(json_get_string(src, "type", NULL));
    move->distance = (float)json_get_number(src, "distance", 0.0);
}

/** Parse an optional projectile component. */
static void parse_projectile(const JsonValue* obj, AbilityProjectileDef* proj) {
    const JsonValue* src = json_get(obj, "projectile");
    if (!src) return;

    proj->type  = parse_projectile_type(json_get_string(src, "type", NULL));
    proj->speed = (float)json_get_number(src, "speed", 0.0);
    proj->width = (float)json_get_number(src, "width", 0.0);
}

/** Parse an optional conditional-damage component. */
static void parse_bonus_damage(const JsonValue* obj, AbilityBonusDamageDef* bonus) {
    const JsonValue* src = json_get(obj, "bonusDamage");
    if (!src) return;

    const char* condition = json_get_string(src, "condition", "");
    if (strcmp(condition, "target_below_hp_percent") == 0) bonus->condition = 1;

    bonus->threshold  = (float)json_get_number(src, "threshold", 0.0);
    bonus->multiplier = (float)json_get_number(src, "multiplier", 1.0);
}

/**
 * Parse the status-effect array.
 *
 * @param key  Ability key, used only to name the ability in warnings.
 * @return     The number of entries populated, limited by max_effects.
 */
static int parse_status_effects(const JsonValue* obj, AbilityEffectDef* effects,
                                int max_effects, const char* key) {
    const JsonValue* arr = json_get(obj, "statusEffects");
    int listed = json_count(arr);
    int count = 0;

    for (int i = 0; i < listed && count < max_effects; i++) {
        const JsonValue* src = json_at(arr, i);

        const char* type_name = json_get_string(src, "type", NULL);
        StatusEffectType type = parse_effect_type(type_name);
        if (type == EFFECT_NONE) {
            fprintf(stderr, "  %s: unknown status effect \"%s\" ignored\n",
                    key, type_name ? type_name : "(missing)");
            continue;
        }

        AbilityEffectDef* eff = &effects[count++];
        memset(eff, 0, sizeof(*eff));

        eff->type         = type;
        eff->value        = json_get_int(src, "value", 0);
        eff->duration     = (float)json_get_number(src, "duration", 0.0);
        eff->tick_rate    = (float)json_get_number(src, "tickRate", 0.0);
        eff->stat         = parse_stat_type(json_get_string(src, "stat", NULL));
        eff->reapply      = (uint8_t)json_get_bool(src, "reapply", 0);
        eff->self         = (uint8_t)json_get_bool(src, "self", 0);
        eff->on_condition = (uint8_t)json_get_bool(src, "onCondition", 0);
    }

    if (listed > max_effects) {
        fprintf(stderr, "  %s: %d status effects exceeds the %d supported; extras ignored\n",
                key, listed, MAX_ABILITY_EFFECTS);
    }
    return count;
}

/**
 * Parse one ability object.
 *
 * @return An owned definition, or NULL when allocation fails.
 */
static AbilityDef* parse_ability(const char* key, const JsonValue* obj) {
    AbilityDef* ability = calloc(1, sizeof(AbilityDef));
    if (!ability) return NULL;

    ability->id = next_ability_id;
    strncpy(ability->key, key, MAX_ABILITY_KEY - 1);

    copy_field(ability->name, MAX_ABILITY_NAME, obj, "name");
    if (!ability->name[0]) strncpy(ability->name, key, MAX_ABILITY_NAME - 1);

    /* An ability names the race that owns it. Human Form's kit names no race at all,
     * which is exactly what makes it universal. */
    const char* race_key = json_get_string(obj, "race", NULL);
    const RaceDef* race = race_key ? race_get_by_key(race_key) : NULL;
    if (race_key && !race) {
        fprintf(stderr, "  %s: unknown race \"%s\"; the ability will belong to no race\n",
                key, race_key);
    }
    ability->race_id = race ? (uint8_t)race->id : 0;

    ability->form         = parse_form(json_get_string(obj, "form", NULL));
    ability->unlock_level = (uint8_t)json_get_int(obj, "unlockLevel", 1);

    /* Human Form is cooldown-only and has no pool, so a cost there is a data error
     * rather than a design choice. Refuse it instead of charging an absent resource. */
    ability->resource_cost = json_get_int(obj, "resourceCost", 0);
    if (ability->form == FORM_HUMAN && ability->resource_cost != 0) {
        fprintf(stderr, "  %s: Human Form has no resource pool; resourceCost %d ignored\n",
                key, ability->resource_cost);
        ability->resource_cost = 0;
    }

    ability->cooldown    = (float)json_get_number(obj, "cooldown", 0.0);
    ability->cast_time   = (float)json_get_number(obj, "castTime", 0.0);
    ability->range       = (float)json_get_number(obj, "range", 0.0);
    ability->target_type = parse_target_type(json_get_string(obj, "targetType", NULL));

    ability->damage       = json_get_int(obj, "damage", 0);
    ability->damage_type  = parse_damage_type(json_get_string(obj, "damageType", NULL));
    ability->damage_stat  = parse_stat_type(json_get_string(obj, "damageStat", NULL));
    ability->healing      = json_get_int(obj, "healing", 0);
    ability->heal_percent = json_get_int(obj, "healPercent", 0);
    ability->heal_self    = (uint8_t)json_get_bool(obj, "healSelf", 0);

    int cleanse = json_get_int(obj, "removesDebuffs", 0);
    if (cleanse > 0) {
        ability->effects[ability->effect_count].type  = EFFECT_CLEANSE;
        ability->effects[ability->effect_count].value = cleanse;
        ability->effect_count++;
    }

    parse_aoe(obj, &ability->aoe);
    ability->effect_count += parse_status_effects(
        obj, &ability->effects[ability->effect_count],
        MAX_ABILITY_EFFECTS - ability->effect_count, key);

    parse_spawn(obj, &ability->spawn);
    parse_movement(obj, &ability->movement);
    parse_projectile(obj, &ability->projectile);
    parse_bonus_damage(obj, &ability->bonus_damage);

    copy_field(ability->animation, sizeof(ability->animation), obj, "animation");
    copy_field(ability->sfx,       sizeof(ability->sfx),       obj, "sfx");
    copy_field(ability->vfx,       sizeof(ability->vfx),       obj, "vfx");
    copy_field(ability->image,     sizeof(ability->image),     obj, "image");

    return ability;
}

/**
 * Initialize the ability registry from a JSON file.
 *
 * Replaces the current registry contents and retains definitions until
 * abilities_cleanup(). Load the race registry first: abilities resolve their owning
 * race by key, and an ability loaded before its race would belong to nothing.
 *
 * @param json_filepath  Path to the ability-definition JSON file.
 * @return               1 on success, or 0 when the file cannot be read or parsed.
 */
int abilities_init(const char* json_filepath) {
    abilities_cleanup();

    const char* err = NULL;
    JsonValue* root = json_parse_file(json_filepath, &err);
    if (!root) {
        fprintf(stderr, "[ABILITIES] %s: %s\n", json_filepath ? json_filepath : "(no path)",
                err ? err : "unreadable");
        return 0;
    }

    /* Abilities resolve their owning race by key, so loading them before the race
     * registry silently orphans every one of them. Say so rather than letting it
     * present later as abilities that belong to nobody. */
    if (race_registry_count() == 0) {
        fprintf(stderr, "[ABILITIES] The race registry is empty — load races.json first, "
                        "or every ability will belong to no race\n");
    }

    const JsonValue* abilities = json_get(root, "abilities");
    if (!abilities) {
        fprintf(stderr, "[ABILITIES] %s: no \"abilities\" object\n", json_filepath);
        json_free(root);
        return 0;
    }

    int listed = json_member_count(abilities);
    for (int i = 0; i < listed; i++) {
        const char* key = json_key_at(abilities, i);
        if (!key || key[0] == '_') continue;   /* leading underscore marks a comment */

        if (next_ability_id >= MAX_ABILITIES) {
            fprintf(stderr, "[ABILITIES] registry full at %d; skipping \"%s\"\n",
                    MAX_ABILITIES, key);
            break;
        }
        if (ability_get_by_key(key)) {
            fprintf(stderr, "[ABILITIES] duplicate key \"%s\" ignored\n", key);
            continue;
        }

        AbilityDef* ability = parse_ability(key, json_member_at(abilities, i));
        if (!ability) continue;

        ability_table[ability->id] = ability;
        abilities_loaded++;
        next_ability_id++;
    }

    json_free(root);
    printf("[ABILITIES] Loaded %d abilities from %s\n", abilities_loaded, json_filepath);
    return 1;
}

/**
 * Retrieve an ability definition by its one-based identifier.
 *
 * The returned pointer remains owned by the ability registry.
 *
 * @return The ability definition, or NULL when the identifier is absent or invalid.
 */
const AbilityDef* ability_get(uint16_t ability_id) {
    if (ability_id == 0 || ability_id >= MAX_ABILITIES) return NULL;
    return ability_table[ability_id];
}

/**
 * Retrieve an ability definition by its JSON key.
 *
 * The returned pointer remains owned by the ability registry.
 *
 * @param key  Terminated ability key; may be NULL.
 * @return     The matching definition, or NULL when no match exists.
 */
const AbilityDef* ability_get_by_key(const char* key) {
    if (!key) return NULL;
    for (int i = 1; i < MAX_ABILITIES; i++) {
        if (ability_table[i] && strcmp(ability_table[i]->key, key) == 0) {
            return ability_table[i];
        }
    }
    return NULL;
}

/**
 * Resolve a spec's ability keys to identifiers, in the order the spec lists them.
 *
 * @return The number written; unresolved keys are reported and skipped.
 */
int ability_resolve_keys(const char keys[][MAX_ABILITY_KEY], int key_count,
                         uint16_t* out_ids, int max_out) {
    if (!keys || !out_ids) return 0;

    int count = 0;
    for (int i = 0; i < key_count && count < max_out; i++) {
        if (!keys[i][0]) continue;

        const AbilityDef* ability = ability_get_by_key(keys[i]);
        if (!ability) {
            fprintf(stderr, "[ABILITIES] spec names \"%s\", which is not defined\n", keys[i]);
            continue;
        }
        out_ids[count++] = ability->id;
    }
    return count;
}

/**
 * Fill the hotbar for one race and form.
 *
 * Animal Form draws from the race's default spec, in the order races.json lists the
 * keys, so a designer controls slot order. Human Form ignores the race entirely.
 *
 * @return The number of identifiers written.
 */
int ability_get_form_abilities(uint8_t race_id, uint8_t form, uint16_t* out_ids, int max_out) {
    if (!out_ids || max_out <= 0) return 0;

    if (form == FORM_HUMAN) {
        /* Every race gets the same five, in load order. Nothing consults race_id. */
        int count = 0;
        for (int i = 1; i < MAX_ABILITIES && count < max_out; i++) {
            if (ability_table[i] && ability_table[i]->form == FORM_HUMAN) {
                out_ids[count++] = ability_table[i]->id;
            }
        }
        return count;
    }

    const RaceDef* race = race_get(race_id);
    const RaceSpec* spec = race_default_spec(race);
    if (!spec) return 0;

    return ability_resolve_keys(spec->ability_keys, spec->ability_count, out_ids, max_out);
}

/**
 * Report the number of registered ability definitions.
 *
 * @return The number of loaded abilities.
 */
int abilities_get_count(void) {
    return abilities_loaded;
}

/**
 * Release all ability definitions and clear the registry.
 */
void abilities_cleanup(void) {
    for (int i = 0; i < MAX_ABILITIES; i++) {
        free(ability_table[i]);
        ability_table[i] = NULL;
    }
    abilities_loaded = 0;
    next_ability_id = 1;
}
