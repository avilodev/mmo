/**
 * @file
 * Derive the runtime behaviour table from the content registry.
 *
 * The adapter between what content authors write and what the AI tick reads, and
 * the reason those are allowed to differ. `npc/abilities.json` states reach in
 * tiles, because that is how enemy_types.txt is written and how an encounter is
 * reasoned about; the tick wants world units. This file is the single place that
 * multiplies, against world_tile_size() rather than against a hardcoded 16 -- so
 * a world whose tile size differs needs no data rewritten.
 *
 * It converts distances and nothing else. Everything an ability *is* stays on
 * the registry row, which is what lets a trigger, an affix and a phase all read
 * one description of the same attack.
 *
 * It also replaces a 230-line hand-rolled JSON scanner that used to live in
 * npc_ai.c, in a file that already included the tested parser it was avoiding.
 * With it went the two compiled caps: the table is sized to the content, so
 * nothing is dropped for being the thirty-third enemy or the fifth ability.
 */

#include "npc_ai.h"
#include "npc_registry.h"
#include "world_collision.h"
#include "log.h"

#include <stdlib.h>
#include <string.h>

static NPCAIProfile*  g_ai_profiles;
static NPCAbilityDef* g_ai_ability_pool;   /* One flat allocation for every profile. */
static int            g_ai_profile_count;

/** Every registry ability, converted once, indexed by registry index.
 *
 * Two callers need an ability nothing in a type's own list names: a `cast`
 * trigger, and a `swap_ability` trigger whose target is a registry row the type
 * never declared -- Alpha Wolf's Double Bite swaps its lunge for `serial_burst`.
 * Converting the whole registry once is 61 rows today and removes the alternative,
 * which is converting tiles to world units somewhere other than this file.
 */
static NPCAbilityDef* g_global_abilities;
static int            g_global_ability_count;

/** The widest ability_count across loaded profiles.
 *
 * npc_ai.c sizes its per-tick shortlist from this rather than from a compiled
 * constant -- which is precisely the bound this change removes.
 */
static int g_widest_kit;

/** Report the registry index a resolved type-ability instance came from.
 *
 * A type's abilities are override-applied *copies*, so the shared row they were
 * copied from is found by key -- which is also what guarantees the wire's
 * ability_id is the same number for every enemy using the same attack.
 *
 * @return The registry index, or -1 when the key does not resolve.
 */
static int base_index_of(const NPCAbilityDefn* inst) {
    const NPCAbilityDefn* base = npc_ability_by_key(inst->key);
    return base ? (int)(base - npc_ability_at(0)) : -1;
}

/** Convert one registry ability's distances into world units.
 *
 * Every delivery is expressible: what a delivery *does* is npc_behavior.c's
 * business, and a row it cannot act on is caught by npc_content_validate()
 * rather than dropped here. This function cannot fail.
 */
static void convert_ability(const NPCAbilityDefn* src, uint16_t ability_id,
                            float tile, NPCAbilityDef* out) {
    memset(out, 0, sizeof(*out));
    out->ability_id = ability_id;
    out->src        = src;

    out->range              = src->range_tiles            * tile;
    out->telegraph_radius   = src->telegraph.radius_tiles * tile;
    out->telegraph_width    = src->telegraph.width_tiles  * tile;
    out->telegraph_length   = src->telegraph.length_tiles * tile;
    out->zone_impact_radius = src->zone_impact.radius_tiles * tile;
    out->zone_self_radius   = src->zone_self.radius_tiles   * tile;
    out->buff_radius        = src->buff.radius_tiles      * tile;
    out->heal_radius        = src->heal.radius_tiles      * tile;
    out->displace_distance  = src->displace.distance_tiles * tile;
    out->summon_spread      = src->summon.spread_tiles    * tile;

    /* A contact attack has no authored reach: it lands when the bodies touch.
     * Giving it one tile here means the act phase can range-check every
     * delivery the same way instead of special-casing this one. */
    if (src->delivery == NPC_DELIV_CONTACT && out->range <= 0.0f)
        out->range = tile;
}

/** Bring a profile's hold distance and stopping distance in line with its kit.
 *
 * An archetype states where an enemy would *like* to stand. Whether it can fight
 * from there is a property of the kit it was given, and the two are authored in
 * different files by different concerns -- so they have to be reconciled
 * somewhere, once, rather than trusted to agree.
 *
 * Two numbers come out of it:
 *
 *  - `preferred_range` drops to the longest reach the kit has, so a kiter whose
 *    only attack is a five-tile cone holds at five rather than at six and
 *    spending the fight out of its own range. This is the engine half of what
 *    R7 used to catch by refusing the content.
 *
 *  - `shortest_reach` records the closest an ability wants the target, so a
 *    chaser closes far enough to use its *shortest* attack rather than stopping
 *    at the distance of its longest.
 *
 * Abilities that need no target are excluded from both: a summon casts from
 * wherever the summoner is standing and says nothing about where that should be.
 */
static void clamp_hold_distance(NPCAIProfile* prof) {
    float longest = 0.0f, shortest = 0.0f;

    for (int i = 0; i < prof->ability_count; i++) {
        const NPCAbilityDef* ab = &prof->abilities[i];
        const NPCAbilityDefn* s = ab->src;
        int self_directed = s->delivery == NPC_DELIV_PASSIVE ||
                            s->delivery == NPC_DELIV_BUFF ||
                            s->delivery == NPC_DELIV_SUMMON ||
                            (s->delivery == NPC_DELIV_ZONE && ab->range <= 0.0f);
        if (self_directed || ab->range <= 0.0f) continue;

        if (ab->range > longest) longest = ab->range;
        if (shortest <= 0.0f || ab->range < shortest) shortest = ab->range;
    }

    prof->shortest_reach = shortest;

    /* A type with nothing targeted keeps its archetype's preference: a pure
     * summoner or healer has no reach to reconcile against. */
    if (longest > 0.0f && prof->preferred_range > longest)
        prof->preferred_range = longest;
}

int npc_ai_init(void) {
    npc_ai_cleanup();

    int types = npc_type_count();
    if (types <= 0) {
        LOG_ERROR("[NPC_AI] the content registry holds no types");
        return 0;
    }

    int total_abilities = 0;
    for (int i = 0; i < types; i++) {
        const NPCTypeDef* t = npc_type_at(i);
        if (t) total_abilities += t->ability_count;
    }

    g_global_ability_count = npc_ability_count();
    g_global_abilities = g_global_ability_count
        ? calloc((size_t)g_global_ability_count, sizeof(*g_global_abilities))
        : NULL;
    if (g_global_ability_count && !g_global_abilities) {
        LOG_ERROR("[NPC_AI] could not allocate the global ability table");
        npc_ai_cleanup();
        return 0;
    }

    g_ai_profiles = calloc((size_t)types, sizeof(*g_ai_profiles));
    g_ai_ability_pool = total_abilities
        ? calloc((size_t)total_abilities, sizeof(*g_ai_ability_pool))
        : NULL;
    if (!g_ai_profiles || (total_abilities && !g_ai_ability_pool)) {
        LOG_ERROR("[NPC_AI] could not allocate a behaviour table for %d types", types);
        npc_ai_cleanup();
        return 0;
    }

    const float tile = world_tile_size();
    int written = 0;
    int failures = 0;

    for (int i = 0; i < g_global_ability_count; i++)
        convert_ability(npc_ability_at(i), (uint16_t)(i + 1), tile,
                        &g_global_abilities[i]);

    for (int i = 0; i < types; i++) {
        const NPCTypeDef* t = npc_type_at(i);
        if (!t) continue;
        const NPCArchetypeDef* arch = npc_archetype_at(t->archetype_index);
        if (!arch) {
            LOG_ERROR("[NPC_AI] '%s' names an archetype that did not resolve", t->key);
            failures++;
            continue;
        }

        NPCAIProfile* prof = &g_ai_profiles[g_ai_profile_count];
        prof->npc_type_id     = t->id;
        prof->type            = t;
        prof->arch            = arch;
        prof->movement        = arch->movement;
        prof->target_priority = t->target_priority >= 0
                              ? (uint8_t)t->target_priority : arch->target_priority;
        prof->move_speed      = t->move_speed > 0 ? t->move_speed : arch->move_speed;

        prof->preferred_range     = arch->preferred_tiles     * tile;
        prof->aggro_range         = arch->aggro_tiles         * tile;
        prof->leash_range         = arch->never_leashes ? 0.0f : arch->leash_tiles * tile;
        prof->retreat_range       = arch->retreat_tiles       * tile;
        prof->ally_affinity_range = arch->ally_affinity_tiles * tile;
        prof->hop_distance        = arch->reactive_hop_tiles  * tile;
        prof->hop_threat_range    = arch->reactive_hop_threat_tiles * tile;
        prof->hop_cooldown        = arch->reactive_hop_cooldown;
        prof->movement_noise      = t->movement_noise > 0.0f
                                  ? t->movement_noise : arch->movement_noise;

        prof->abilities     = &g_ai_ability_pool[written];
        prof->ability_count = 0;
        prof->shortest_reach = 0.0f;

        for (int s = 0; s < t->ability_count; s++) {
            const NPCAbilityDefn* a = npc_type_ability(t, s);
            if (!a) continue;
            int base = base_index_of(a);
            if (base < 0) {
                LOG_ERROR("[NPC_AI] '%s' slot %d references ability '%s', which the "
                          "registry no longer holds", t->key, s, a->key);
                failures++;
                continue;
            }
            /* +1 so zero stays "no ability" on the wire, as the client expects. */
            convert_ability(a, (uint16_t)(base + 1), tile, &g_ai_ability_pool[written]);
            written++;
            prof->ability_count++;
        }

        clamp_hold_distance(prof);
        g_ai_profile_count++;
    }

    if (failures > 0) {
        LOG_ERROR("[NPC_AI] %d archetype resolution failure(s); refusing to "
                  "serve a partially expressible roster", failures);
        npc_ai_cleanup();
        return 0;
    }

    for (int i = 0; i < g_ai_profile_count; i++)
        if (g_ai_profiles[i].ability_count > g_widest_kit)
            g_widest_kit = g_ai_profiles[i].ability_count;

    LOG_INFO("[NPC_AI] %d behaviour profiles built from the registry, %d abilities, "
             "widest kit %d, tile size %.0f",
             g_ai_profile_count, written, g_widest_kit, (double)tile);
    return 1;
}

void npc_ai_cleanup(void) {
    free(g_ai_profiles);      g_ai_profiles = NULL;
    free(g_ai_ability_pool);  g_ai_ability_pool = NULL;
    free(g_global_abilities); g_global_abilities = NULL;
    g_ai_profile_count = 0;
    g_global_ability_count = 0;
    g_widest_kit = 0;
}

const NPCAbilityDef* npc_ai_registry_ability(int registry_index) {
    if (registry_index < 0 || registry_index >= g_global_ability_count) return NULL;
    return &g_global_abilities[registry_index];
}

int npc_ai_widest_kit(void) { return g_widest_kit; }

const NPCAIProfile* npc_ai_get_profile(uint16_t npc_type_id) {
    for (int i = 0; i < g_ai_profile_count; i++)
        if (g_ai_profiles[i].npc_type_id == npc_type_id) return &g_ai_profiles[i];
    return NULL;
}
