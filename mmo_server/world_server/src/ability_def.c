// ============================================================================
// ability_def.c — JSON loader for ability definitions
//
// Same architecture as items_database.c:
//   - Read file into memory
//   - Walk the JSON manually (no library)
//   - Store into a static array for O(1) lookup by ID
// ============================================================================

#include "ability_def.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

// ---------------------------------------------------------------------------
// Static storage
// ---------------------------------------------------------------------------

static AbilityDef* ability_table[MAX_ABILITIES];   // Indexed by numeric ID
static int abilities_loaded = 0;
static uint16_t next_ability_id = 1;               // Auto-assigned, 1-based

// ---------------------------------------------------------------------------
// Forward declarations — JSON helpers
// ---------------------------------------------------------------------------

static char* read_file(const char* filepath);
static const char* find_json_value(const char* json, const char* key);
static int parse_json_string(const char* val, char* out, int out_size);
static int parse_json_int(const char* val);
static float parse_json_float(const char* val);
static const char* find_json_object(const char* json, const char* key, int* out_len);
static const char* find_json_array(const char* json, const char* key);

// Parsers for sub-objects
static void parse_aoe(const char* obj_json, AbilityAoeDef* aoe);
static void parse_spawn(const char* obj_json, AbilitySpawnDef* spawn);
static void parse_movement(const char* obj_json, AbilityMovementDef* move);
static void parse_projectile(const char* obj_json, AbilityProjectileDef* proj);
static void parse_bonus_damage(const char* obj_json, AbilityBonusDamageDef* bonus);
static int  parse_status_effects(const char* obj_json, AbilityEffectDef* effects, int max_effects);
static int  parse_abilities_json(const char* json_content);

// ---------------------------------------------------------------------------
// API implementation
// ---------------------------------------------------------------------------

int abilities_init(const char* json_filepath) {
    printf("Loading abilities from: %s\n", json_filepath);

    memset(ability_table, 0, sizeof(ability_table));
    abilities_loaded = 0;
    next_ability_id = 1;

    char* json_content = read_file(json_filepath);
    if (!json_content) {
        fprintf(stderr, "Failed to read abilities file: %s\n", json_filepath);
        return 0;
    }

    int result = parse_abilities_json(json_content);
    free(json_content);

    if (result) {
        printf("Successfully loaded %d abilities\n", abilities_loaded);
    } else {
        fprintf(stderr, "Failed to parse abilities JSON\n");
    }

    return result;
}

const AbilityDef* ability_get(uint16_t ability_id) {
    if (ability_id == 0 || ability_id >= MAX_ABILITIES) return NULL;
    return ability_table[ability_id];
}

const AbilityDef* ability_get_by_key(const char* key) {
    if (!key) return NULL;
    for (int i = 1; i < MAX_ABILITIES; i++) {
        if (ability_table[i] && strcmp(ability_table[i]->key, key) == 0) {
            return ability_table[i];
        }
    }
    return NULL;
}

int ability_get_class_abilities(uint8_t class_id, uint16_t* out_ids, int max_out) {
    int count = 0;
    for (int i = 1; i < MAX_ABILITIES && count < max_out; i++) {
        if (ability_table[i] && ability_table[i]->class_id == class_id) {
            out_ids[count++] = ability_table[i]->id;
        }
    }
    return count;
}

int abilities_get_count(void) {
    return abilities_loaded;
}

void abilities_cleanup(void) {
    for (int i = 0; i < MAX_ABILITIES; i++) {
        if (ability_table[i]) {
            free(ability_table[i]);
            ability_table[i] = NULL;
        }
    }
    abilities_loaded = 0;
    printf("Abilities system cleaned up\n");
}

// ---------------------------------------------------------------------------
// JSON helpers (same style as items_database.c)
// ---------------------------------------------------------------------------

static char* read_file(const char* filepath) {
    FILE* f = fopen(filepath, "rb");
    if (!f) return NULL;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    char* buffer = malloc(size + 1);
    if (!buffer) { fclose(f); return NULL; }

    fread(buffer, 1, size, f);
    buffer[size] = '\0';
    fclose(f);

    return buffer;
}

// Find "key": <value> and return pointer to <value>
static const char* find_json_value(const char* json, const char* key) {
    char search[128];
    snprintf(search, sizeof(search), "\"%s\"", key);

    const char* pos = strstr(json, search);
    if (!pos) return NULL;

    pos = strchr(pos, ':');
    if (!pos) return NULL;
    pos++;

    while (*pos && isspace(*pos)) pos++;
    return pos;
}

// Extract a JSON string value into out. Returns 1 on success.
static int parse_json_string(const char* val, char* out, int out_size) {
    if (!val || *val != '"') return 0;
    const char* end = strchr(val + 1, '"');
    if (!end) return 0;

    int len = end - (val + 1);
    if (len >= out_size) len = out_size - 1;
    memcpy(out, val + 1, len);
    out[len] = '\0';
    return 1;
}

static int parse_json_int(const char* val) {
    if (!val) return 0;
    return atoi(val);
}

static float parse_json_float(const char* val) {
    if (!val) return 0.0f;
    return (float)atof(val);
}

// Find a nested JSON object: "key": { ... }
// Returns pointer to the '{', sets *out_len to include the closing '}'.
static const char* find_json_object(const char* json, const char* key, int* out_len) {
    const char* val = find_json_value(json, key);
    if (!val || *val != '{') return NULL;

    int brace = 0;
    const char* end = val;
    while (*end) {
        if (*end == '{') brace++;
        if (*end == '}') { brace--; if (brace == 0) break; }
        end++;
    }
    if (brace != 0) return NULL;

    if (out_len) *out_len = (int)(end - val + 1);
    return val;
}

// Find a JSON array: "key": [ ... ]
// Returns pointer to the '['
static const char* find_json_array(const char* json, const char* key) {
    const char* val = find_json_value(json, key);
    if (!val || *val != '[') return NULL;
    return val;
}

// ---------------------------------------------------------------------------
// Sub-object parsers
// ---------------------------------------------------------------------------

static AbilityAoeShape parse_aoe_shape(const char* str) {
    if (strcmp(str, "circle") == 0)    return ABILITY_AOE_CIRCLE;
    if (strcmp(str, "sphere") == 0)    return ABILITY_AOE_CIRCLE; // treat sphere as circle in 2D
    if (strcmp(str, "cone") == 0)      return ABILITY_AOE_CONE;
    if (strcmp(str, "rectangle") == 0) return ABILITY_AOE_RECTANGLE;
    return ABILITY_AOE_NONE;
}

static AbilityDamageType parse_damage_type(const char* str) {
    if (strcmp(str, "physical") == 0) return ABILITY_DMG_PHYSICAL;
    if (strcmp(str, "earth") == 0)    return ABILITY_DMG_EARTH;
    if (strcmp(str, "spirit") == 0)   return ABILITY_DMG_SPIRIT;
    return ABILITY_DMG_PHYSICAL;
}

static AbilityTargetType parse_target_type(const char* str) {
    if (strcmp(str, "ally") == 0)   return ABILITY_TARGET_ALLY;
    if (strcmp(str, "self") == 0)   return ABILITY_TARGET_SELF;
    if (strcmp(str, "ground") == 0) return ABILITY_TARGET_GROUND;
    return ABILITY_TARGET_ENEMY;
}

static StatusEffectType parse_effect_type(const char* str) {
    if (strcmp(str, "dot") == 0)     return EFFECT_DOT;
    if (strcmp(str, "hot") == 0)     return EFFECT_HOT;
    if (strcmp(str, "stun") == 0)    return EFFECT_STUN;
    if (strcmp(str, "slow") == 0)    return EFFECT_SLOW;
    if (strcmp(str, "buff") == 0)    return EFFECT_BUFF;
    if (strcmp(str, "stealth") == 0) return EFFECT_STEALTH;
    if (strcmp(str, "knockup") == 0) return EFFECT_KNOCKUP;
    if (strcmp(str, "link") == 0)    return EFFECT_LINK;
    if (strcmp(str, "cleanse") == 0) return EFFECT_CLEANSE;
    return EFFECT_NONE;
}

static StatType parse_stat_type(const char* str) {
    if (strcmp(str, "damage") == 0)  return STAT_DAMAGE;
    if (strcmp(str, "defense") == 0) return STAT_DEFENSE;
    if (strcmp(str, "speed") == 0)   return STAT_SPEED;
    return STAT_NONE;
}

static SpawnEntityType parse_spawn_type(const char* str) {
    if (strcmp(str, "wall") == 0)  return SPAWN_WALL;
    if (strcmp(str, "zone") == 0)  return SPAWN_ZONE;
    if (strcmp(str, "decoy") == 0) return SPAWN_DECOY;
    return SPAWN_NONE;
}

static AbilityMovementType parse_movement_type(const char* str) {
    if (strcmp(str, "teleport") == 0) return MOVEMENT_TELEPORT;
    if (strcmp(str, "dash") == 0)     return MOVEMENT_DASH;
    return MOVEMENT_NONE;
}

static AbilityProjectileType parse_projectile_type(const char* str) {
    if (strcmp(str, "linear") == 0) return PROJECTILE_LINEAR;
    return PROJECTILE_NONE;
}

static void parse_aoe(const char* obj_json, AbilityAoeDef* aoe) {
    // Extract the "aoe" sub-object
    int obj_len = 0;
    const char* aoe_obj = find_json_object(obj_json, "aoe", &obj_len);
    if (!aoe_obj) return;

    char* sub = malloc(obj_len + 1);
    memcpy(sub, aoe_obj, obj_len);
    sub[obj_len] = '\0';

    // Shape
    const char* shape_val = find_json_value(sub, "shape");
    if (shape_val) {
        char shape_str[32] = {0};
        parse_json_string(shape_val, shape_str, sizeof(shape_str));
        aoe->shape = parse_aoe_shape(shape_str);
    }

    // Radius
    const char* radius_val = find_json_value(sub, "radius");
    if (radius_val) aoe->radius = parse_json_float(radius_val);

    // Angle (for cones)
    const char* angle_val = find_json_value(sub, "angle");
    if (angle_val) aoe->angle = parse_json_float(angle_val);

    // Width/height (for rectangles)
    const char* width_val = find_json_value(sub, "width");
    if (width_val) aoe->width = parse_json_float(width_val);

    const char* height_val = find_json_value(sub, "height");
    if (height_val) aoe->height = parse_json_float(height_val);

    free(sub);
}

static void parse_spawn(const char* obj_json, AbilitySpawnDef* spawn) {
    int obj_len = 0;
    const char* spawn_obj = find_json_object(obj_json, "spawnsEntity", &obj_len);
    if (!spawn_obj) return;

    char* sub = malloc(obj_len + 1);
    memcpy(sub, spawn_obj, obj_len);
    sub[obj_len] = '\0';

    const char* type_val = find_json_value(sub, "type");
    if (type_val) {
        char type_str[32] = {0};
        parse_json_string(type_val, type_str, sizeof(type_str));
        spawn->type = parse_spawn_type(type_str);
    }

    const char* dur_val = find_json_value(sub, "duration");
    if (dur_val) spawn->duration = parse_json_float(dur_val);

    const char* col_val = find_json_value(sub, "hasCollision");
    if (col_val) spawn->has_collision = (strncmp(col_val, "true", 4) == 0) ? 1 : 0;

    const char* hp_val = find_json_value(sub, "hp");
    if (hp_val) spawn->hp = parse_json_int(hp_val);

    const char* inherit_val = find_json_value(sub, "inheritsAppearance");
    if (inherit_val) spawn->inherits_appearance = (strncmp(inherit_val, "true", 4) == 0) ? 1 : 0;

    free(sub);
}

static void parse_movement(const char* obj_json, AbilityMovementDef* move) {
    int obj_len = 0;
    const char* move_obj = find_json_object(obj_json, "movement", &obj_len);
    if (!move_obj) return;

    char* sub = malloc(obj_len + 1);
    memcpy(sub, move_obj, obj_len);
    sub[obj_len] = '\0';

    const char* type_val = find_json_value(sub, "type");
    if (type_val) {
        char type_str[32] = {0};
        parse_json_string(type_val, type_str, sizeof(type_str));
        move->type = parse_movement_type(type_str);
    }

    const char* dist_val = find_json_value(sub, "distance");
    if (dist_val) move->distance = parse_json_float(dist_val);

    free(sub);
}

static void parse_projectile(const char* obj_json, AbilityProjectileDef* proj) {
    int obj_len = 0;
    const char* proj_obj = find_json_object(obj_json, "projectile", &obj_len);
    if (!proj_obj) return;

    char* sub = malloc(obj_len + 1);
    memcpy(sub, proj_obj, obj_len);
    sub[obj_len] = '\0';

    const char* type_val = find_json_value(sub, "type");
    if (type_val) {
        char type_str[32] = {0};
        parse_json_string(type_val, type_str, sizeof(type_str));
        proj->type = parse_projectile_type(type_str);
    }

    const char* speed_val = find_json_value(sub, "speed");
    if (speed_val) proj->speed = parse_json_float(speed_val);

    const char* width_val = find_json_value(sub, "width");
    if (width_val) proj->width = parse_json_float(width_val);

    free(sub);
}

static void parse_bonus_damage(const char* obj_json, AbilityBonusDamageDef* bonus) {
    int obj_len = 0;
    const char* bonus_obj = find_json_object(obj_json, "bonusDamage", &obj_len);
    if (!bonus_obj) return;

    char* sub = malloc(obj_len + 1);
    memcpy(sub, bonus_obj, obj_len);
    sub[obj_len] = '\0';

    const char* cond_val = find_json_value(sub, "condition");
    if (cond_val) {
        char cond_str[64] = {0};
        parse_json_string(cond_val, cond_str, sizeof(cond_str));
        if (strcmp(cond_str, "target_below_hp_percent") == 0) {
            bonus->condition = 1;
        }
    }

    const char* thresh_val = find_json_value(sub, "threshold");
    if (thresh_val) bonus->threshold = parse_json_float(thresh_val);

    const char* mult_val = find_json_value(sub, "multiplier");
    if (mult_val) bonus->multiplier = parse_json_float(mult_val);

    free(sub);
}

// Parse statusEffects — can be an array of objects or an array of strings.
// Objects: [{"type":"hot","value":5,"duration":4,"tickRate":1}]
// Strings: ["Recklessness","Bleed"] — stored as named effects (effect_type = NONE,
//          but you can map string names to types here if you want).
static int parse_status_effects(const char* obj_json, AbilityEffectDef* effects, int max_effects) {
    const char* arr = find_json_array(obj_json, "statusEffects");
    if (!arr) return 0;

    int count = 0;
    const char* pos = arr + 1; // skip '['

    while (*pos && *pos != ']' && count < max_effects) {
        while (*pos && isspace(*pos)) pos++;

        if (*pos == '{') {
            // Object-style effect
            int brace = 0;
            const char* obj_start = pos;
            const char* obj_end = pos;
            while (*obj_end) {
                if (*obj_end == '{') brace++;
                if (*obj_end == '}') { brace--; if (brace == 0) break; }
                obj_end++;
            }

            int obj_len = (int)(obj_end - obj_start + 1);
            char* sub = malloc(obj_len + 1);
            memcpy(sub, obj_start, obj_len);
            sub[obj_len] = '\0';

            AbilityEffectDef* eff = &effects[count];
            memset(eff, 0, sizeof(*eff));

            // type
            const char* type_val = find_json_value(sub, "type");
            if (type_val) {
                char type_str[32] = {0};
                parse_json_string(type_val, type_str, sizeof(type_str));
                eff->type = parse_effect_type(type_str);
            }

            // value
            const char* val = find_json_value(sub, "value");
            if (val) eff->value = parse_json_int(val);

            // duration
            const char* dur = find_json_value(sub, "duration");
            if (dur) eff->duration = parse_json_float(dur);

            // tickRate
            const char* tick = find_json_value(sub, "tickRate");
            if (tick) eff->tick_rate = parse_json_float(tick);

            // stat (for buffs)
            const char* stat_val = find_json_value(sub, "stat");
            if (stat_val) {
                char stat_str[32] = {0};
                parse_json_string(stat_val, stat_str, sizeof(stat_str));
                eff->stat = parse_stat_type(stat_str);
            }

            // reapply
            const char* reapply_val = find_json_value(sub, "reapply");
            if (reapply_val) eff->reapply = (strncmp(reapply_val, "true", 4) == 0) ? 1 : 0;

            free(sub);
            count++;
            pos = obj_end + 1;

        } else if (*pos == '"') {
            // String-style effect name (e.g. "Bleed", "Recklessness")
            // Map known names to effect types
            const char* end = strchr(pos + 1, '"');
            if (!end) break;

            int len = (int)(end - (pos + 1));
            char name[32] = {0};
            if (len < 32) memcpy(name, pos + 1, len);

            AbilityEffectDef* eff = &effects[count];
            memset(eff, 0, sizeof(*eff));

            // Map known string names
            if (strcmp(name, "Bleed") == 0) {
                eff->type = EFFECT_DOT;
                eff->value = 5;
                eff->duration = 6.0f;
                eff->tick_rate = 1.0f;
            } else if (strcmp(name, "Recklessness") == 0) {
                eff->type = EFFECT_BUFF;
                eff->stat = STAT_DAMAGE;
                eff->value = 25;
                eff->duration = 6.0f;
            } else {
                // Unknown — store as generic buff with the name
                eff->type = EFFECT_BUFF;
                eff->duration = 5.0f;
            }

            count++;
            pos = end + 1;
        } else {
            pos++;
        }

        // Skip comma
        while (*pos && (*pos == ',' || isspace(*pos))) pos++;
    }

    return count;
}

// ---------------------------------------------------------------------------
// Class name -> ID mapping
// ---------------------------------------------------------------------------

static uint8_t parse_class_id(const char* str) {
    if (strcmp(str, "gladiator") == 0)  return 1;
    if (strcmp(str, "ninja") == 0)      return 2;
    if (strcmp(str, "landweaver") == 0) return 3;
    if (strcmp(str, "spirit") == 0)     return 4;
    return 0;
}

// ---------------------------------------------------------------------------
// Main parser — walks the "abilities" object
// ---------------------------------------------------------------------------

static int parse_abilities_json(const char* json_content) {
    // Find the "abilities" object: "abilities": { "cleave": {...}, "rage": {...}, ... }
    const char* abilities_start = strstr(json_content, "\"abilities\"");
    if (!abilities_start) {
        fprintf(stderr, "No 'abilities' object found in JSON\n");
        return 0;
    }

    // Find the opening brace of the abilities object
    const char* obj_start = strchr(abilities_start + 11, '{');
    if (!obj_start) return 0;

    const char* pos = obj_start + 1;

    while (*pos) {
        // Skip whitespace
        while (*pos && isspace(*pos)) pos++;

        if (*pos == '}') break; // End of abilities object

        // Expect a key string: "cleave"
        if (*pos != '"') { pos++; continue; }

        // Read the ability key
        const char* key_end = strchr(pos + 1, '"');
        if (!key_end) break;

        char ability_key[MAX_ABILITY_KEY] = {0};
        int key_len = (int)(key_end - (pos + 1));
        if (key_len >= MAX_ABILITY_KEY) key_len = MAX_ABILITY_KEY - 1;
        memcpy(ability_key, pos + 1, key_len);

        // Skip past key and find the colon
        pos = key_end + 1;
        while (*pos && *pos != ':') pos++;
        if (!*pos) break;
        pos++; // skip ':'

        // Skip whitespace to find '{'
        while (*pos && isspace(*pos)) pos++;
        if (*pos != '{') { pos++; continue; }

        // Find matching closing brace
        int brace = 0;
        const char* ab_start = pos;
        const char* ab_end = pos;
        while (*ab_end) {
            if (*ab_end == '{') brace++;
            if (*ab_end == '}') { brace--; if (brace == 0) break; }
            ab_end++;
        }
        if (brace != 0) break;

        // Extract this ability object
        int ab_len = (int)(ab_end - ab_start + 1);
        char* ab_json = malloc(ab_len + 1);
        memcpy(ab_json, ab_start, ab_len);
        ab_json[ab_len] = '\0';

        // --- Parse the ability ---
        AbilityDef* ability = calloc(1, sizeof(AbilityDef));
        ability->id = next_ability_id;
        strncpy(ability->key, ability_key, MAX_ABILITY_KEY - 1);

        // name
        const char* name_val = find_json_value(ab_json, "name");
        if (name_val) parse_json_string(name_val, ability->name, MAX_ABILITY_NAME);

        // class
        const char* class_val = find_json_value(ab_json, "class");
        if (class_val) {
            char class_str[32] = {0};
            parse_json_string(class_val, class_str, sizeof(class_str));
            ability->class_id = parse_class_id(class_str);
        }

        // unlockLevel
        const char* level_val = find_json_value(ab_json, "unlockLevel");
        if (level_val) ability->unlock_level = (uint8_t)parse_json_int(level_val);

        // manaCost
        const char* mana_val = find_json_value(ab_json, "manaCost");
        if (mana_val) ability->mana_cost = parse_json_int(mana_val);

        // cooldown
        const char* cd_val = find_json_value(ab_json, "cooldown");
        if (cd_val) ability->cooldown = parse_json_float(cd_val);

        // castTime
        const char* ct_val = find_json_value(ab_json, "castTime");
        if (ct_val) ability->cast_time = parse_json_float(ct_val);

        // range
        const char* range_val = find_json_value(ab_json, "range");
        if (range_val) ability->range = parse_json_float(range_val);

        // targetType
        const char* target_val = find_json_value(ab_json, "targetType");
        if (target_val) {
            char target_str[32] = {0};
            parse_json_string(target_val, target_str, sizeof(target_str));
            ability->target_type = parse_target_type(target_str);
        }

        // damage
        const char* dmg_val = find_json_value(ab_json, "damage");
        if (dmg_val) ability->damage = parse_json_int(dmg_val);

        // damageType
        const char* dtype_val = find_json_value(ab_json, "damageType");
        if (dtype_val) {
            char dtype_str[32] = {0};
            parse_json_string(dtype_val, dtype_str, sizeof(dtype_str));
            ability->damage_type = parse_damage_type(dtype_str);
        }

        // healing
        const char* heal_val = find_json_value(ab_json, "healing");
        if (heal_val) ability->healing = parse_json_int(heal_val);

        // removesDebuffs (for cleanse — store as effect)
        const char* cleanse_val = find_json_value(ab_json, "removesDebuffs");
        if (cleanse_val && parse_json_int(cleanse_val) > 0) {
            ability->effects[ability->effect_count].type = EFFECT_CLEANSE;
            ability->effects[ability->effect_count].value = parse_json_int(cleanse_val);
            ability->effect_count++;
        }

        // aoe (sub-object)
        parse_aoe(ab_json, &ability->aoe);

        // statusEffects (array)
        int se_count = parse_status_effects(ab_json, 
                                            &ability->effects[ability->effect_count],
                                            MAX_ABILITY_EFFECTS - ability->effect_count);
        ability->effect_count += se_count;

        // spawnsEntity (sub-object)
        parse_spawn(ab_json, &ability->spawn);

        // movement (sub-object)
        parse_movement(ab_json, &ability->movement);

        // projectile (sub-object)
        parse_projectile(ab_json, &ability->projectile);

        // bonusDamage (sub-object)
        parse_bonus_damage(ab_json, &ability->bonus_damage);

        // animation, sfx, vfx
        const char* anim_val = find_json_value(ab_json, "animation");
        if (anim_val) parse_json_string(anim_val, ability->animation, sizeof(ability->animation));

        const char* sfx_val = find_json_value(ab_json, "sfx");
        if (sfx_val) parse_json_string(sfx_val, ability->sfx, sizeof(ability->sfx));

        const char* vfx_val = find_json_value(ab_json, "vfx");
        if (vfx_val) parse_json_string(vfx_val, ability->vfx, sizeof(ability->vfx));

        // --- Store it ---
        if (ability->id < MAX_ABILITIES) {
            ability_table[ability->id] = ability;
            abilities_loaded++;
            next_ability_id++;

            printf("  Loaded: [%u] %s (%s) — class=%u, level=%u, cd=%.1fs, cast=%.1fs\n",
                   ability->id, ability->name, ability->key,
                   ability->class_id, ability->unlock_level,
                   ability->cooldown, ability->cast_time);
        } else {
            free(ability);
            fprintf(stderr, "  Ability ID overflow, skipping '%s'\n", ability_key);
        }

        free(ab_json);

        // Move past this object
        pos = ab_end + 1;
        while (*pos && (*pos == ',' || isspace(*pos))) pos++;
    }

    return 1;
}