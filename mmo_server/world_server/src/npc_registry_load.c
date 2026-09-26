/**
 * @file
 * Parse the five NPC content files into the registry's tables.
 *
 * The lookup half of this module lives in npc_registry.c; the two share storage
 * through npc_registry_internal.h. They are split because together they cross the
 * 800-line ceiling, not because they are separable concerns -- nothing here is
 * reachable except through npc_registry_load().
 *
 * Order matters and is enforced by npcreg_load_all():
 *
 *   factions -> archetypes -> abilities -> affixes -> types -> summon back-patch
 *
 * Summons are the one cycle. An ability names the types it spawns, and types load
 * last because they reference abilities, so summon targets stay keys until types
 * exist and are resolved afterwards.
 */

#include "npc_registry_internal.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** Resolve an effect name to a StatusEffectType, or EFFECT_NONE when unknown. */
static StatusEffectType parse_effect_type(const char* s) {
    static const char* const names[] = {
        "none", "dot", "hot", "stun", "slow", "buff", "stealth", "knockup", "link",
        "cleanse", "taunt", "resource", "damage_taken", "damage_dealt", "root",
        "channel", "hot_percent",
        "blind", "fear", "charm", "form_lock", "mark"
    };
    int n = (int)(sizeof(names) / sizeof(names[0]));
    return (StatusEffectType)npcreg_lookup_name(s, names, n, EFFECT_NONE);
}

/* --- Effects ------------------------------------------------------------- */

/** Append an ability's effect array to the shared table.
 *
 * The registry is immutable after load, so a projectile or zone carries a span into
 * this table rather than copying four AbilityEffectDefs into every pooled struct.
 */
static void parse_effects(const JsonValue* owner, const char* file, const char* key,
                          int* out_first, int* out_count) {
    *out_first = g_npcreg_effect_count;
    *out_count = 0;

    const JsonValue* arr = json_get(owner, "effects");
    int n = json_count(arr);
    for (int i = 0; i < n; i++) {
        const JsonValue* e = json_at(arr, i);
        if (!NPCREG_RESERVE(g_npcreg_effects, g_npcreg_effect_cap, g_npcreg_effect_count)) {
            npcreg_fail(file, key, "out of memory growing the effect table");
            return;
        }
        AbilityEffectDef* d = &g_npcreg_effects[g_npcreg_effect_count];
        memset(d, 0, sizeof(*d));

        const char* tname = json_get_string(e, "type", "none");
        d->type = parse_effect_type(tname);
        if (d->type == EFFECT_NONE)
            npcreg_fail(file, key, "effect %d names unknown type '%s'", i, tname);

        d->duration  = (float)json_get_number(e, "duration", 0);
        d->tick_rate = (float)json_get_number(e, "tick_rate", 0);
        d->value     = json_get_int(e, "value", 0);
        d->stat      = STAT_TARGET_NONE;
        d->reapply   = (uint8_t)json_get_bool(e, "reapply", 0);
        d->self      = (uint8_t)json_get_bool(e, "self", 0);

        g_npcreg_effect_count++;
        (*out_count)++;
    }
}

/* --- Component parsing --------------------------------------------------- */

static void parse_telegraph(const JsonValue* o, NPCAbilityDefn* a) {
    const JsonValue* t = json_get(o, "telegraph");
    if (!t) return;
    a->has_telegraph = 1;
    a->telegraph.shape = (uint8_t)npcreg_lookup_name(json_get_string(t, "shape", "circle"),
                                              g_npcreg_shape_names, 4, NPC_TELE_CIRCLE);
    a->telegraph.radius_tiles = (float)json_get_number(t, "radius_tiles", 0);
    a->telegraph.angle        = (float)json_get_number(t, "angle", 0);
    a->telegraph.width_tiles  = (float)json_get_number(t, "width_tiles", 0);
    a->telegraph.length_tiles = (float)json_get_number(t, "length_tiles", 0);
    a->telegraph.at_target    = (uint8_t)json_get_bool(t, "at_target", 0);
    a->telegraph.teleport_on_resolve = (uint8_t)json_get_bool(t, "teleport_on_resolve", 0);
    a->telegraph.resolve_mode = (uint8_t)npcreg_lookup_name(json_get_string(t, "resolve_mode", "damage"),
                                                     g_npcreg_resolve_names, 4, NPC_RESOLVE_DAMAGE);
}

static void parse_zone(const JsonValue* z, const char* file, const char* key,
                       NPCZoneSpec* out) {
    out->radius_tiles = (float)json_get_number(z, "radius_tiles", 1);
    out->duration     = (float)json_get_number(z, "duration", 0);
    out->tick_rate    = (float)json_get_number(z, "tick_rate", 1);
    out->count        = json_get_int(z, "count", 1);
    out->per_tick     = (uint8_t)json_get_bool(z, "per_tick", 0);
    parse_effects(z, file, key, &out->effect_first, &out->effect_count);
}

static void parse_ability_components(const JsonValue* o, NPCAbilityDefn* a,
                                     NPCSummonKeys* sk, const char* file) {
    parse_telegraph(o, a);

    const JsonValue* p = json_get(o, "projectile");
    if (p) {
        a->has_projectile = 1;
        a->projectile.speed = (float)json_get_number(p, "speed", 400);
        a->projectile.width = (float)json_get_number(p, "width", 8);
    }

    const JsonValue* b = json_get(o, "burst");
    if (b) {
        a->has_burst = 1;
        a->burst.count        = json_get_int(b, "count", 1);
        a->burst.interval     = (float)json_get_number(b, "interval", 0);
        a->burst.spread_angle = (float)json_get_number(b, "spread_angle", 0);
    }

    const JsonValue* zi = json_get(o, "zone_on_impact");
    if (zi) { a->has_zone_impact = 1; parse_zone(zi, file, a->key, &a->zone_impact); }

    const JsonValue* zs = json_get(o, "zone_on_self");
    if (zs) { a->has_zone_self = 1; parse_zone(zs, file, a->key, &a->zone_self); }

    const JsonValue* s = json_get(o, "summon");
    if (s) {
        a->has_summon = 1;
        a->summon.count_min    = json_get_int(s, "count_min", 1);
        a->summon.count_max    = json_get_int(s, "count_max", 1);
        a->summon.delay        = (float)json_get_number(s, "delay", 0);
        a->summon.spread_tiles = (float)json_get_number(s, "spread_tiles", 1);

        const JsonValue* types = json_get(s, "types");
        int n = json_count(types);
        if (n > NPC_SUMMON_TYPES_MAX) {
            npcreg_fail(file, a->key, "summon names %d types, past the %d this build holds",
                 n, NPC_SUMMON_TYPES_MAX);
            n = NPC_SUMMON_TYPES_MAX;
        }
        for (int i = 0; i < n; i++) {
            const char* k = json_as_string(json_at(types, i), "");
            snprintf(sk->keys[i], NPC_KEY_MAX, "%s", k);
            a->summon.type_index[i] = -1;
        }
        sk->count = n;
        a->summon.type_count = n;
    }

    const JsonValue* bf = json_get(o, "buff");
    if (bf) {
        a->has_buff = 1;
        a->buff.radius_tiles = (float)json_get_number(bf, "radius_tiles", 0);
        a->buff.duration     = (float)json_get_number(bf, "duration", 0);
        const char* tgt = json_get_string(bf, "target", "allies");
        a->buff.target_self  = (uint8_t)(strcmp(tgt, "self") == 0);
        a->buff.faction_only = (uint8_t)(strcmp(tgt, "faction") == 0);
        a->buff.single_ally      = (uint8_t)json_get_bool(bf, "single_ally", 0);
        a->buff.ready_allies     = (uint8_t)json_get_bool(bf, "ready_allies", 0);
        a->buff.retarget_allies  = (uint8_t)json_get_bool(bf, "retarget", 0);
        parse_effects(bf, file, a->key, &a->buff.effect_first, &a->buff.effect_count);
    }

    const JsonValue* d = json_get(o, "displace");
    if (d) {
        a->has_displace = 1;
        a->displace.distance_tiles = (float)json_get_number(d, "distance_tiles", 1);
        const char* m = json_get_string(d, "mode", "dash");
        a->displace.mode = (uint8_t)(strcmp(m, "teleport") == 0  ? 1 :
                                     strcmp(m, "hop") == 0       ? 2 :
                                     strcmp(m, "ally_swap") == 0 ? 3 : 0);
        a->displace.damage_on_contact = (uint8_t)json_get_bool(d, "damage_on_contact", 0);
        a->displace.terrain_stagger   = (uint8_t)json_get_bool(d, "terrain_stagger", 0);
        a->displace.behind_target     = (uint8_t)json_get_bool(d, "behind_target", 0);
    }

    const JsonValue* h = json_get(o, "heal");
    if (h) {
        a->has_heal = 1;
        a->heal.amount       = json_get_int(h, "amount", 0);
        a->heal.percent      = json_get_int(h, "percent", 0);
        a->heal.radius_tiles = (float)json_get_number(h, "radius_tiles", 0);
        a->heal.lowest_ally_only =
            (uint8_t)(strcmp(json_get_string(h, "target", "allies"), "lowest_ally") == 0);
    }

    const JsonValue* sc = json_get(o, "self_cost");
    if (sc) a->self_cost_health_percent = json_get_int(sc, "health_percent", 0);
}

/* --- File loaders -------------------------------------------------------- */

/** Open one content file and return its root array member, or NULL. */
JsonValue* npcreg_open_file(const char* dir, const char* name, const char* member,
                            const JsonValue** out_arr) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", dir, name);

    const char* err = NULL;
    JsonValue* root = json_parse_file(path, &err);
    if (!root) {
        LOG_ERROR("[NPC_REGISTRY] %s: %s", path, err ? err : "could not be read");
        g_npcreg_errors++;
        return NULL;
    }
    const JsonValue* arr = json_get(root, member);
    if (json_type(arr) != JSON_ARRAY) {
        LOG_ERROR("[NPC_REGISTRY] %s: no '%s' array at the top level", path, member);
        g_npcreg_errors++;
        json_free(root);
        return NULL;
    }
    *out_arr = arr;
    return root;
}

static int load_factions(const char* dir) {
    const JsonValue* arr = NULL;
    JsonValue* root = npcreg_open_file(dir, "factions.json", "factions", &arr);
    if (!root) return 0;

    g_npcreg_faction_count = json_count(arr);
    g_npcreg_factions = calloc((size_t)g_npcreg_faction_count, sizeof(*g_npcreg_factions));
    if (!g_npcreg_factions) { json_free(root); return 0; }

    for (int i = 0; i < g_npcreg_faction_count; i++) {
        const JsonValue* o = json_at(arr, i);
        NPCFactionDef* f = &g_npcreg_factions[i];
        npcreg_copy_key(f->key,  sizeof(f->key),  o, "key",  "");
        npcreg_copy_key(f->name, sizeof(f->name), o, "name", "");
        f->id = json_get_int(o, "id", 0);
        const JsonValue* c = json_get(o, "color");
        for (int k = 0; k < 3; k++)
            f->color[k] = (float)json_as_number(json_at(c, k), 0.5);
        if (!f->key[0]) npcreg_fail("factions.json", NULL, "row %d has no key", i);
    }
    json_free(root);
    return 1;
}

static int load_archetypes(const char* dir) {
    const JsonValue* arr = NULL;
    JsonValue* root = npcreg_open_file(dir, "archetypes.json", "archetypes", &arr);
    if (!root) return 0;

    g_npcreg_archetype_count = json_count(arr);
    g_npcreg_archetypes = calloc((size_t)g_npcreg_archetype_count, sizeof(*g_npcreg_archetypes));
    if (!g_npcreg_archetypes) { json_free(root); return 0; }

    for (int i = 0; i < g_npcreg_archetype_count; i++) {
        const JsonValue* o = json_at(arr, i);
        NPCArchetypeDef* a = &g_npcreg_archetypes[i];
        npcreg_copy_key(a->key, sizeof(a->key), o, "key", "");
        a->movement = (uint8_t)npcreg_lookup_name(json_get_string(o, "movement", "follow"),
                                           g_npcreg_movement_names, 3, NPC_ARCH_FOLLOW);
        a->move_speed          = (float)json_get_number(o, "move_speed", 0);
        a->aggro_tiles         = (float)json_get_number(o, "aggro_tiles", 0);
        a->leash_tiles         = (float)json_get_number(o, "leash_tiles", 0);
        a->preferred_tiles     = (float)json_get_number(o, "preferred_tiles", 0);
        a->retreat_tiles       = (float)json_get_number(o, "retreat_tiles", 0);
        a->ally_affinity_tiles = (float)json_get_number(o, "ally_affinity_tiles", 0);
        a->flee_while_adds_alive = (uint8_t)json_get_bool(o, "flee_while_adds_alive", 0);
        a->movement_cc_immune    = (uint8_t)json_get_bool(o, "movement_cc_immune", 0);
        a->never_leashes         = (uint8_t)json_get_bool(o, "never_leashes", 0);
        a->target_priority = (uint8_t)npcreg_lookup_name(json_get_string(o, "target_priority", "nearest"),
                                                  g_npcreg_priority_names, 4, NPC_PRIORITY_NEAREST);
        a->reactive_hop_tiles        = (float)json_get_number(o, "reactive_hop_tiles", 0);
        a->reactive_hop_cooldown     = (float)json_get_number(o, "reactive_hop_cooldown", 0);
        a->reactive_hop_threat_tiles = (float)json_get_number(o, "reactive_hop_threat_tiles", 0);
        a->movement_noise = (float)json_get_number(o, "movement_noise", 0);
        a->hitbox_radius  = (float)json_get_number(o, "hitbox_radius", 16.0);
        if (!a->key[0]) npcreg_fail("archetypes.json", NULL, "row %d has no key", i);
    }
    json_free(root);
    return 1;
}

static int load_abilities(const char* dir) {
    const JsonValue* arr = NULL;
    JsonValue* root = npcreg_open_file(dir, "abilities.json", "abilities", &arr);
    if (!root) return 0;

    g_npcreg_ability_count = json_count(arr);
    g_npcreg_abilities   = calloc((size_t)g_npcreg_ability_count, sizeof(*g_npcreg_abilities));
    g_npcreg_summon_keys = calloc((size_t)g_npcreg_ability_count, sizeof(*g_npcreg_summon_keys));
    if (!g_npcreg_abilities || !g_npcreg_summon_keys) { json_free(root); return 0; }

    for (int i = 0; i < g_npcreg_ability_count; i++) {
        const JsonValue* o = json_at(arr, i);
        NPCAbilityDefn* a = &g_npcreg_abilities[i];
        npcreg_copy_key(a->key,  sizeof(a->key),  o, "key",  "");
        npcreg_copy_key(a->name, sizeof(a->name), o, "name", a->key);

        const char* dname = json_get_string(o, "delivery", "");
        a->delivery = (uint8_t)npcreg_lookup_name(dname, g_npcreg_delivery_names, NPC_DELIV_COUNT, 0xFF);
        if (a->delivery == 0xFF) {
            npcreg_fail("abilities.json", a->key, "unknown delivery '%s'", dname);
            a->delivery = NPC_DELIV_CONTACT;
        }

        a->cast_time   = (float)json_get_number(o, "cast_time", 0);
        a->cooldown    = (float)json_get_number(o, "cooldown", 0);
        a->range_tiles = (float)json_get_number(o, "range_tiles", 0);
        a->recovery    = (float)json_get_number(o, "recovery", 0);
        a->damage      = json_get_int(o, "damage", 0);
        a->damage_type = (uint8_t)npcreg_lookup_name(json_get_string(o, "damage_type", "physical"),
                                              g_npcreg_dmgtype_names, 3, ABILITY_DMG_PHYSICAL);

        parse_ability_components(o, a, &g_npcreg_summon_keys[i], "abilities.json");
        parse_effects(o, "abilities.json", a->key, &a->effect_first, &a->effect_count);

        if (!a->key[0]) npcreg_fail("abilities.json", NULL, "row %d has no key", i);
    }
    json_free(root);
    return 1;
}

/** Parse one trigger row into the shared trigger pool. Returns 1 when stored. */
int npcreg_parse_trigger(const JsonValue* o, const NPCTypeDef* owner,
                         const char* file, const char* key) {
    if (!NPCREG_RESERVE(g_npcreg_triggers, g_npcreg_trigger_cap, g_npcreg_trigger_count)) return 0;
    NPCTriggerDef* t = &g_npcreg_triggers[g_npcreg_trigger_count];
    memset(t, 0, sizeof(*t));
    t->ability_index = t->from_index = t->to_index = t->phase_index = -1;

    const char* wname = json_get_string(o, "when", "");
    t->when = (uint8_t)npcreg_lookup_name(wname, g_npcreg_when_names, NPC_WHEN_COUNT, 0xFF);
    if (t->when == 0xFF) { npcreg_fail(file, key, "trigger names unknown event '%s'", wname); t->when = 0; }

    const char* aname = json_get_string(o, "action", "modify");
    t->action = (uint8_t)npcreg_lookup_name(aname, g_npcreg_action_names, NPC_DO_COUNT, 0xFF);
    if (t->action == 0xFF) { npcreg_fail(file, key, "trigger names unknown action '%s'", aname); t->action = 0; }

    t->value = (float)json_get_number(o, "value",
               json_get_number(o, "value_tiles", 0));
    /* hp_below latches by default; a timer that latched would fire once and stop. */
    t->once      = (uint8_t)json_get_bool(o, "once", t->when == NPC_WHEN_HP_BELOW);
    t->repeating = (uint8_t)json_get_bool(o, "repeating", 0);

    const JsonValue* m = json_get(o, "modifiers");
    if (m) {
        t->move_speed_pct     = (float)json_get_number(m, "move_speed_pct", 0);
        t->attack_speed_pct   = (float)json_get_number(m, "attack_speed_pct", 0);
        t->damage_pct         = (float)json_get_number(m, "damage_pct", 0);
        t->damage_taken_pct   = (float)json_get_number(m, "damage_taken_pct", 0);
        t->cooldown_pct       = (float)json_get_number(m, "cooldown_pct", 0);
        t->phase_duration_pct = (float)json_get_number(m, "phase_duration_pct", 0);
    }

    /* Ability references resolve against the global registry; swap targets resolve
     * against the owning type's own slots, which is why they are indices not keys. */
    const char* ab = json_get_string(o, "ability", "");
    if (ab[0]) {
        const NPCAbilityDefn* found = npc_ability_by_key(ab);
        if (!found) npcreg_fail(file, key, "trigger casts unknown ability '%s'", ab);
        else t->ability_index = (int)(found - g_npcreg_abilities);
    }
    /* set_phase names a phase this type declares. Phases load before triggers,
     * which is what lets the reference resolve here rather than at runtime. */
    const char* ph = json_get_string(o, "phase", "");
    if (ph[0] && owner) {
        for (int p = 0; p < owner->phase_count; p++)
            if (strcmp(g_npcreg_phases[owner->phase_first + p].key, ph) == 0)
                t->phase_index = p;
        if (t->phase_index < 0)
            npcreg_fail(file, key, "trigger sets phase '%s', which this type does not declare", ph);
    }

    const char* from = json_get_string(o, "from", "");
    const char* to   = json_get_string(o, "to", "");
    if ((from[0] || to[0]) && owner) {
        for (int s = 0; s < owner->ability_count; s++) {
            const NPCAbilityDefn* inst = &g_npcreg_type_abilities[owner->ability_first + s];
            if (from[0] && strcmp(inst->key, from) == 0) t->from_index = s;
        }
        if (to[0]) {
            const NPCAbilityDefn* f = npc_ability_by_key(to);
            if (!f) npcreg_fail(file, key, "trigger swaps to unknown ability '%s'", to);
            else t->to_index = (int)(f - g_npcreg_abilities);
        }
        if (from[0] && t->from_index < 0)
            npcreg_fail(file, key, "trigger swaps from '%s', which this type does not have", from);
    }

    g_npcreg_trigger_count++;
    return 1;
}

static int load_affixes(const char* dir) {
    const JsonValue* arr = NULL;
    JsonValue* root = npcreg_open_file(dir, "affixes.json", "affixes", &arr);
    if (!root) return 0;

    g_npcreg_affix_count = json_count(arr);
    g_npcreg_affixes = calloc((size_t)(g_npcreg_affix_count ? g_npcreg_affix_count : 1), sizeof(*g_npcreg_affixes));
    if (!g_npcreg_affixes) { json_free(root); return 0; }

    for (int i = 0; i < g_npcreg_affix_count; i++) {
        const JsonValue* o = json_at(arr, i);
        NPCAffixDef* a = &g_npcreg_affixes[i];
        npcreg_copy_key(a->key,  sizeof(a->key),  o, "key",  "");
        npcreg_copy_key(a->name, sizeof(a->name), o, "name", a->key);
        npcreg_copy_key(a->name_prefix, sizeof(a->name_prefix), o, "name_prefix", "");
        npcreg_copy_key(a->name_suffix, sizeof(a->name_suffix), o, "name_suffix", "");

        const JsonValue* ap = json_get(o, "applies_to");
        const JsonValue* roles = json_get(ap, "roles");
        for (int k = 0; k < json_count(roles); k++) {
            int r = npcreg_lookup_name(json_as_string(json_at(roles, k), ""), g_npcreg_role_names,
                                NPC_ROLE_COUNT, -1);
            if (r < 0) npcreg_fail("affixes.json", a->key, "applies_to names unknown role");
            else a->role_mask |= (1u << r);
        }
        const JsonValue* facs = json_get(ap, "factions");
        for (int k = 0; k < json_count(facs); k++) {
            const char* fk = json_as_string(json_at(facs, k), "");
            int found = -1;
            for (int f = 0; f < g_npcreg_faction_count; f++)
                if (strcmp(g_npcreg_factions[f].key, fk) == 0) found = f;
            if (found < 0) npcreg_fail("affixes.json", a->key, "applies_to names unknown faction '%s'", fk);
            else a->faction_mask |= (1u << found);
        }

        const JsonValue* m = json_get(o, "modifiers");
        a->cast_time_pct    = (float)json_get_number(m, "cast_time_pct", 0);
        a->damage_pct       = (float)json_get_number(m, "damage_pct", 0);
        a->move_speed_pct   = (float)json_get_number(m, "move_speed_pct", 0);
        a->attack_speed_pct = (float)json_get_number(m, "attack_speed_pct", 0);
        a->damage_taken_pct = (float)json_get_number(m, "damage_taken_pct", 0);

        /* An affix may carry triggers of its own -- Duelist's counter window is
         * one -- and they are evaluated as a continuation of the type's list, so
         * the latch bitset has to be sized for both. compute_maxima() does that. */
        a->trigger_first = g_npcreg_trigger_count;
        a->trigger_count = 0;
        const JsonValue* trg = json_get(o, "triggers");
        for (int k = 0; k < json_count(trg); k++)
            if (npcreg_parse_trigger(json_at(trg, k), NULL, "affixes.json", a->key))
                a->trigger_count++;

        const JsonValue* ol = json_get(o, "outline");
        if (json_type(ol) == JSON_ARRAY) {
            a->has_outline = 1;
            for (int k = 0; k < 3; k++)
                a->outline[k] = (float)json_as_number(json_at(ol, k), 1.0);
        }
        if (!a->key[0]) npcreg_fail("affixes.json", NULL, "row %d has no key", i);
    }
    json_free(root);
    return 1;
}

int npcreg_load_all(const char* data_dir) {
    if (!load_factions(data_dir))   return 0;
    if (!load_archetypes(data_dir)) return 0;
    if (!load_abilities(data_dir))  return 0;
    if (!load_affixes(data_dir))    return 0;
    if (!npcreg_load_types(data_dir)) return 0;
    npcreg_resolve_summons();
    return 1;
}
