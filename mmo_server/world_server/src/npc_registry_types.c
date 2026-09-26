/**
 * @file
 * Parse types.json: the composed half of the content registry.
 *
 * Split from npc_registry_load.c when the two together crossed the project's
 * 800-line ceiling, at a seam that was already there. The other half reads the
 * four files describing *components* -- factions, archetypes, abilities and
 * affixes -- each of which stands alone and resolves nothing. This one reads the
 * file that **composes** them, which is why every cross-reference, every scalar
 * override and the model's one genuine cycle live here.
 *
 * That cycle is summons: an ability names the types it calls, and a type names
 * the abilities it carries. It is broken by holding summon targets as keys
 * through the load and resolving them once at the end, when both tables exist.
 *
 * Neither file is a module of its own. They share npc_registry_internal.h and
 * are one unit split for size -- the arrangement npc_registry.c already uses,
 * and the one common/include/net_reactor_internal.h established.
 */

#include "npc_registry_internal.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** Apply this type's scalar overrides onto a copy of a base ability.
 *
 * The allowlist is the point. A type may retune an ability -- damage, timing, counts,
 * extent -- but may not change its *shape*: delivery, telegraph geometry and effects
 * are what make an attack recognisable, and an override that changed them would be a
 * second definition wearing the first one's name.
 */
static void apply_overrides(NPCAbilityDefn* inst, NPCSummonKeys* inst_summon,
                            const JsonValue* ref,
                            const char* file, const char* key) {
    static const char* const allowed[] = {
        "key", "damage", "cooldown", "cast_time", "range_tiles", "recovery",
        "count", "interval", "spread_angle", "count_min", "count_max", "delay",
        "duration", "radius_tiles", "distance_tiles", "amount", "percent",
        /* Not a scalar, and allowed anyway, for a stated reason: five summoners
         * share one `summon_adds` row and each calls something different. What a
         * summon brings is not its *shape* -- the cast, the delay, the telegraph
         * a player reads are all unchanged -- so it is the one list an override
         * may replace. Everything else in the allowlist above is a number. */
        "types"
    };
    int n_allowed = (int)(sizeof(allowed) / sizeof(allowed[0]));

    int members = json_member_count(ref);
    for (int i = 0; i < members; i++) {
        const char* k = json_key_at(ref, i);
        if (!k) continue;
        if (npcreg_lookup_name(k, allowed, n_allowed, -1) < 0) {
            npcreg_fail(file, key, "override '%s' on '%s' is not a scalar; shape may not be "
                            "overridden -- give it its own ability row", k, inst->key);
            continue;
        }
        const JsonValue* v = json_member_at(ref, i);
        double num = json_as_number(v, 0);

        if (strcmp(k, "damage") == 0)             inst->damage = (int)num;
        else if (strcmp(k, "cooldown") == 0)      inst->cooldown = (float)num;
        else if (strcmp(k, "cast_time") == 0)     inst->cast_time = (float)num;
        else if (strcmp(k, "range_tiles") == 0)   inst->range_tiles = (float)num;
        else if (strcmp(k, "recovery") == 0)      inst->recovery = (float)num;
        else if (strcmp(k, "count") == 0)         inst->burst.count = (int)num;
        else if (strcmp(k, "interval") == 0)      inst->burst.interval = (float)num;
        else if (strcmp(k, "spread_angle") == 0)  inst->burst.spread_angle = (float)num;
        else if (strcmp(k, "count_min") == 0)     inst->summon.count_min = (int)num;
        else if (strcmp(k, "count_max") == 0)     inst->summon.count_max = (int)num;
        else if (strcmp(k, "delay") == 0)         inst->summon.delay = (float)num;
        else if (strcmp(k, "amount") == 0)        inst->heal.amount = (int)num;
        else if (strcmp(k, "percent") == 0)       inst->heal.percent = (int)num;
        else if (strcmp(k, "duration") == 0) {
            if (inst->has_buff)        inst->buff.duration = (float)num;
            if (inst->has_zone_impact) inst->zone_impact.duration = (float)num;
            if (inst->has_zone_self)   inst->zone_self.duration = (float)num;
        } else if (strcmp(k, "radius_tiles") == 0) {
            if (inst->has_buff)        inst->buff.radius_tiles = (float)num;
            if (inst->has_zone_impact) inst->zone_impact.radius_tiles = (float)num;
            if (inst->has_zone_self)   inst->zone_self.radius_tiles = (float)num;
            if (inst->has_heal)        inst->heal.radius_tiles = (float)num;
        } else if (strcmp(k, "distance_tiles") == 0) {
            inst->displace.distance_tiles = (float)num;
        } else if (strcmp(k, "types") == 0) {
            if (!inst_summon) continue;
            int count = json_count(v);
            if (count > NPC_SUMMON_TYPES_MAX) {
                npcreg_fail(file, key, "'%s' overrides %d summon types; the cap is %d",
                            inst->key, count, NPC_SUMMON_TYPES_MAX);
                count = NPC_SUMMON_TYPES_MAX;
            }
            memset(inst_summon, 0, sizeof(*inst_summon));
            for (int c = 0; c < count; c++) {
                const char* tk = json_as_string(json_at(v, c), "");
                snprintf(inst_summon->keys[c], NPC_KEY_MAX, "%s", tk);
            }
            inst_summon->count = count;
            inst->summon.type_count = count;
            for (int c = 0; c < NPC_SUMMON_TYPES_MAX; c++)
                inst->summon.type_index[c] = -1;
        }
    }
}

/** Resolve one type's ability list, applying overrides into the instance pool. */
static void load_type_abilities(const JsonValue* o, NPCTypeDef* t) {
    const JsonValue* arr = json_get(o, "abilities");
    int n = json_count(arr);

    t->ability_first = g_npcreg_type_ability_count;
    t->ability_count = 0;

    for (int i = 0; i < n; i++) {
        const JsonValue* ref = json_at(arr, i);
        const char* key = (json_type(ref) == JSON_STRING)
                        ? json_as_string(ref, "")
                        : json_get_string(ref, "key", "");

        const NPCAbilityDefn* base = npc_ability_by_key(key);
        if (!base) {
            npcreg_fail("types.json", t->key, "references unknown ability '%s'", key);
            continue;
        }
        if (!NPCREG_RESERVE(g_npcreg_type_abilities, g_npcreg_type_ability_cap, g_npcreg_type_ability_count) ||
            !NPCREG_RESERVE(g_npcreg_type_summon_keys, g_npcreg_type_summon_cap, g_npcreg_type_ability_count)) {
            npcreg_fail("types.json", t->key, "out of memory growing the ability instance pool");
            return;
        }
        NPCAbilityDefn* inst = &g_npcreg_type_abilities[g_npcreg_type_ability_count];
        NPCSummonKeys* inst_summon = &g_npcreg_type_summon_keys[g_npcreg_type_ability_count];
        *inst = *base;
        /* Inherit the shared row's summon list, so a type that overrides nothing
         * calls what the ability says it calls. */
        *inst_summon = g_npcreg_summon_keys[base - g_npcreg_abilities];
        if (json_type(ref) == JSON_OBJECT)
            apply_overrides(inst, inst_summon, ref, "types.json", t->key);

        g_npcreg_type_ability_count++;
        t->ability_count++;
    }
}

/** Resolve one type's phases, mapping their ability keys to type-local slots. */
static void load_type_phases(const JsonValue* o, NPCTypeDef* t) {
    const JsonValue* arr = json_get(o, "phases");
    int n = json_count(arr);

    t->phase_first = g_npcreg_phase_count;
    t->phase_count = 0;

    for (int i = 0; i < n; i++) {
        const JsonValue* p = json_at(arr, i);
        if (!NPCREG_RESERVE(g_npcreg_phases, g_npcreg_phase_cap, g_npcreg_phase_count)) return;

        NPCPhaseDef* ph = &g_npcreg_phases[g_npcreg_phase_count];
        memset(ph, 0, sizeof(*ph));
        npcreg_copy_key(ph->key, sizeof(ph->key), p, "key", "");

        const JsonValue* ex = json_get(p, "exit");
        if (json_get(ex, "after"))              { ph->exit_kind = NPC_PHASE_AFTER;
                                                  ph->exit_value = (float)json_get_number(ex, "after", 0); }
        else if (json_get(ex, "after_uses"))    { ph->exit_kind = NPC_PHASE_AFTER_USES;
                                                  ph->exit_value = (float)json_get_number(ex, "after_uses", 0); }
        else if (json_get(ex, "target_within")) { ph->exit_kind = NPC_PHASE_TARGET_WITHIN;
                                                  ph->exit_value = (float)json_get_number(ex, "target_within", 0); }
        else if (json_get(ex, "target_beyond")) { ph->exit_kind = NPC_PHASE_TARGET_BEYOND;
                                                  ph->exit_value = (float)json_get_number(ex, "target_beyond", 0); }
        else npcreg_fail("types.json", t->key, "phase '%s' has no exit condition", ph->key);

        ph->slot_first = g_npcreg_phase_slot_count;
        ph->slot_count = 0;

        const JsonValue* abs = json_get(p, "abilities");
        int an = json_count(abs);
        for (int k = 0; k < an; k++) {
            const char* key = json_as_string(json_at(abs, k), "");
            int slot = -1;
            for (int s = 0; s < t->ability_count; s++)
                if (strcmp(g_npcreg_type_abilities[t->ability_first + s].key, key) == 0) slot = s;
            if (slot < 0) {
                npcreg_fail("types.json", t->key,
                     "phase '%s' names ability '%s', which this type does not have",
                     ph->key, key);
                continue;
            }
            if (!NPCREG_RESERVE(g_npcreg_phase_slots, g_npcreg_phase_slot_cap, g_npcreg_phase_slot_count)) return;
            g_npcreg_phase_slots[g_npcreg_phase_slot_count++] = slot;
            ph->slot_count++;
        }

        g_npcreg_phase_count++;
        t->phase_count++;
    }
}

static void load_type_mitigation(const JsonValue* o, NPCTypeDef* t) {
    const JsonValue* m = json_get(o, "mitigation");
    if (!m) return;
    t->has_mitigation = 1;
    NPCMitigationDef* g = &t->mitigation;

    const JsonValue* sh = json_get(m, "shield");
    if (sh) {
        g->has_shield = 1;
        g->shield_damage_taken_pct = (float)json_get_number(sh, "damage_taken_pct", 0);
        g->broken_damage_taken_pct = (float)json_get_number(sh, "broken_damage_taken_pct", 0);
        g->down_seconds  = (float)json_get_number(sh, "down_seconds", 0);
        g->recharge_never =
            (uint8_t)(strcmp(json_get_string(sh, "recharge", "never"), "never") == 0);

        const JsonValue* br = json_get(sh, "break");
        g->break_mode = (uint8_t)npcreg_lookup_name(json_get_string(br, "mode", "none"),
                                             g_npcreg_break_names, 6, NPC_BREAK_NONE);
        g->break_hits    = json_get_int(br, "hits", 0);
        g->break_window  = (float)json_get_number(br, "window", 0);
        g->break_pool_hp = json_get_int(br, "pool_hp", 0);
    }

    const JsonValue* fb = json_get(m, "frontal_block");
    if (fb) {
        g->has_frontal_block = 1;
        g->frontal_arc_degrees = (float)json_get_number(fb, "arc_degrees", 180);
        g->blocked_damage_pct  = (float)json_get_number(fb, "blocked_damage_pct", 100);
    }

    const JsonValue* po = json_get(m, "positional");
    if (po) {
        g->has_positional = 1;
        g->positional_radius_tiles     = (float)json_get_number(po, "radius_tiles", 0);
        g->positional_damage_taken_pct = (float)json_get_number(po, "damage_taken_pct", 0);
    }
}

int npcreg_load_types(const char* dir) {
    const JsonValue* arr = NULL;
    JsonValue* root = npcreg_open_file(dir, "types.json", "npc_types", &arr);
    if (!root) return 0;

    /* Bands are content, not code: widening one is an edit to types.json. */
    const JsonValue* ranges = json_get(root, "id_ranges");
    if (ranges) {
        const char* names[3] = { "quest", "enemy", "summon" };
        int* bands[3] = { g_npcreg_band_quest, g_npcreg_band_enemy, g_npcreg_band_summon };
        for (int b = 0; b < 3; b++) {
            const JsonValue* r = json_get(ranges, names[b]);
            if (json_type(r) != JSON_ARRAY) continue;
            bands[b][0] = json_as_int(json_at(r, 0), bands[b][0]);
            bands[b][1] = json_as_int(json_at(r, 1), bands[b][1]);
        }
    }

    g_npcreg_type_count = json_count(arr);
    g_npcreg_types = calloc((size_t)g_npcreg_type_count, sizeof(*g_npcreg_types));
    if (!g_npcreg_types) { json_free(root); return 0; }

    for (int i = 0; i < g_npcreg_type_count; i++) {
        const JsonValue* o = json_at(arr, i);
        NPCTypeDef* t = &g_npcreg_types[i];
        npcreg_copy_key(t->key,  sizeof(t->key),  o, "key",  "");
        npcreg_copy_key(t->name, sizeof(t->name), o, "name", t->key);
        t->id = (uint16_t)json_get_int(o, "id", 0);

        const char* fk = json_get_string(o, "faction", "");
        t->faction_index = -1;
        for (int f = 0; f < g_npcreg_faction_count; f++)
            if (strcmp(g_npcreg_factions[f].key, fk) == 0) t->faction_index = f;
        if (t->faction_index < 0) npcreg_fail("types.json", t->key, "unknown faction '%s'", fk);

        const char* ak = json_get_string(o, "archetype", "");
        t->archetype_index = -1;
        for (int a = 0; a < g_npcreg_archetype_count; a++)
            if (strcmp(g_npcreg_archetypes[a].key, ak) == 0) t->archetype_index = a;
        if (t->archetype_index < 0) npcreg_fail("types.json", t->key, "unknown archetype '%s'", ak);

        const char* rk = json_get_string(o, "role", "melee");
        int r = npcreg_lookup_name(rk, g_npcreg_role_names, NPC_ROLE_COUNT, -1);
        if (r < 0) { npcreg_fail("types.json", t->key, "unknown role '%s'", rk); r = NPC_ROLE_MELEE; }
        t->role = (uint8_t)r;

        t->health        = json_get_int(o, "health", 0);
        t->armor         = json_get_int(o, "armor", 0);
        t->xp_reward     = json_get_int(o, "xp_reward", 0);
        t->hitbox_radius = (float)json_get_number(o, "hitbox_radius", 0);
        t->move_speed    = (float)json_get_number(o, "move_speed", 0);
        t->movement_noise = (float)json_get_number(o, "movement_noise", 0);

        /* -1 rather than 0 for "unset": 0 is nearest, which is a real choice. */
        t->target_priority = -1;
        const char* tp = json_get_string(o, "target_priority", "");
        if (tp[0]) {
            int p = npcreg_lookup_name(tp, g_npcreg_priority_names, 4, -1);
            if (p < 0) npcreg_fail("types.json", t->key,
                                   "unknown target_priority '%s'", tp);
            else t->target_priority = p;
        }
        t->summon_only   = (uint8_t)json_get_bool(o, "summon_only", 0);
        npcreg_copy_key(t->loot_table, sizeof(t->loot_table), o, "loot_table", t->key);

        const JsonValue* st = json_get(o, "stealth");
        if (st) {
            t->stealth = 1;
            t->stealth_damageable = (uint8_t)json_get_bool(st, "damageable", 1);
            t->stealth_revealed_seconds = (float)json_get_number(st, "revealed_seconds", 3);
        }

        load_type_abilities(o, t);
        load_type_phases(o, t);
        load_type_mitigation(o, t);

        /* Triggers last: swap targets resolve against slots the ability pass created. */
        t->trigger_first = g_npcreg_trigger_count;
        t->trigger_count = 0;
        const JsonValue* trg = json_get(o, "triggers");
        for (int k = 0; k < json_count(trg); k++)
            if (npcreg_parse_trigger(json_at(trg, k), t, "types.json", t->key)) t->trigger_count++;

        t->affix_first = g_npcreg_affix_ref_count;
        t->affix_count = 0;
        const JsonValue* afx = json_get(o, "affixes");
        for (int k = 0; k < json_count(afx); k++) {
            const char* key = json_as_string(json_at(afx, k), "");
            int found = -1;
            for (int a = 0; a < g_npcreg_affix_count; a++)
                if (strcmp(g_npcreg_affixes[a].key, key) == 0) found = a;
            if (found < 0) { npcreg_fail("types.json", t->key, "unknown affix '%s'", key); continue; }
            if (!NPCREG_RESERVE(g_npcreg_affix_refs, g_npcreg_affix_ref_cap, g_npcreg_affix_ref_count)) break;
            g_npcreg_affix_refs[g_npcreg_affix_ref_count++] = found;
            t->affix_count++;
        }

        if (!t->key[0]) npcreg_fail("types.json", NULL, "row %d has no key", i);
    }
    json_free(root);
    return 1;
}

/** Resolve summon target keys now that types exist. */
void npcreg_resolve_summons(void) {
    for (int i = 0; i < g_npcreg_ability_count; i++) {
        if (!g_npcreg_abilities[i].has_summon) continue;
        NPCSummonKeys* sk = &g_npcreg_summon_keys[i];
        for (int k = 0; k < sk->count; k++) {
            const NPCTypeDef* t = npc_type_get_by_key(sk->keys[k]);
            if (!t) {
                npcreg_fail("abilities.json", g_npcreg_abilities[i].key,
                     "summons unknown type '%s'", sk->keys[k]);
                continue;
            }
            g_npcreg_abilities[i].summon.type_index[k] = (int)(t - g_npcreg_types);
        }
    }
    /* Instances resolve their own key list, which is the shared row's unless the
     * type overrode it -- so a Grave Caller raises the dead and a Packmaster
     * calls wolves from the same ability row. */
    for (int i = 0; i < g_npcreg_type_ability_count; i++) {
        NPCAbilityDefn* inst = &g_npcreg_type_abilities[i];
        if (!inst->has_summon) continue;
        NPCSummonKeys* sk = &g_npcreg_type_summon_keys[i];
        inst->summon.type_count = 0;
        for (int k = 0; k < sk->count; k++) {
            const NPCTypeDef* t = npc_type_get_by_key(sk->keys[k]);
            if (!t) {
                npcreg_fail("types.json", inst->key,
                     "summons unknown type '%s'", sk->keys[k]);
                continue;
            }
            inst->summon.type_index[inst->summon.type_count++] = (int)(t - g_npcreg_types);
        }
    }
}
