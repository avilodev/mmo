/**
 * @file
 * Apply, expire and collect NPC status effects, and answer ally queries.
 *
 * player_effects.c's mirror. Where the two differ, the difference is storage:
 * a player's slots live on ActivePlayer, an NPC's live on the pool's parallel
 * array and are reached with the slot lock the NPC already needs. The rule they
 * share is the important one -- modifiers are collected by walking the slots on
 * every read, never maintained as a running total.
 */

#include "npc_effects.h"
#include "npc_registry.h"
#include "log.h"

#include <math.h>
#include <string.h>
#include <time.h>

/** Floor on the movement multiplier.
 *
 * Slows sum in percentage space, so three 40% slows would otherwise stop an NPC
 * outright and leave it standing wherever it happened to be -- unreachable behind
 * terrain, or parked on a quest objective. A floor keeps every slow meaningful
 * while keeping the enemy findable.
 */
#define NPC_SPEED_FLOOR 0.10f

int npc_faction_of_type(uint16_t npc_type_id) {
    const NPCTypeDef* t = npc_type_get(npc_type_id);
    return t ? t->faction_index : -1;
}

/* --- Stealth (V10) -------------------------------------------------------- */

int npc_stealth_hidden(const NPCEntity* npc, double now) {
    if (!npc || !npc->stealth_active) return 0;
    return now >= npc->stealth_revealed_until;
}

void npc_stealth_reveal(NPCEntity* npc, double now) {
    if (!npc || !npc->stealth_active) return;
    const NPCTypeDef* t = npc_type_get(npc->npc_type_id);
    float window = (t && t->stealth_revealed_seconds > 0.0f)
                 ? t->stealth_revealed_seconds : 3.0f;
    double until = now + window;
    if (until > npc->stealth_revealed_until) npc->stealth_revealed_until = until;
}

int npc_stealth_damageable(const NPCEntity* npc, double now) {
    if (!npc_stealth_hidden(npc, now)) return 1;
    const NPCTypeDef* t = npc_type_get(npc->npc_type_id);
    return !t || t->stealth_damageable;
}

/* --- Application ---------------------------------------------------------- */

/** Return the monotonic clock the NPC's absolute expiries are measured against. */
static double now_monotonic(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

int npc_effect_apply(NPCWorld* world, int slot, NPCEntity* npc,
                     const AbilityEffectDef* effect, uint32_t source_id) {
    if (!effect || effect->type == EFFECT_NONE || !npc) return 0;

    /* Taunt and charm are not slots. Both are "who does this NPC fight", which
     * NPCEntity already answers with a source and an absolute expiry -- storing
     * them as effects too would give one question two answers. */
    if (effect->type == EFFECT_TAUNT) {
        npc->taunt_source_id  = source_id;
        npc->taunt_expires_at = now_monotonic() + effect->duration;
        npc->ai_target_id     = source_id;
        return 1;
    }
    /* Re-entering stealth is the whole of Vanish. Expressed as the same
     * StatusEffectType the player side uses, resolved against the hidden-state
     * field NPCEntity already carries rather than into an effect slot. */
    if (effect->type == EFFECT_STEALTH) {
        npc->stealth_active = 1;
        npc->stealth_revealed_until = 0.0;
        return 1;
    }
    if (effect->type == EFFECT_CHARM) {
        npc->charm_source_id  = source_id;
        npc->charm_expires_at = now_monotonic() + effect->duration;
        return 1;
    }

    NPCEffect* fx = npc_world_effects(world, slot);
    int slots = npc_world_max_effect_slots(world);
    if (!fx || slots <= 0) return 0;

    /* Cleanse is instant and clears rather than occupies a slot. */
    if (effect->type == EFFECT_CLEANSE) {
        for (int i = 0; i < slots; i++) {
            if (!fx[i].active) continue;
            uint8_t t = fx[i].effect_type;
            int harmful = (t == EFFECT_DOT || t == EFFECT_SLOW || t == EFFECT_STUN ||
                           t == EFFECT_ROOT || t == EFFECT_CHANNEL || t == EFFECT_MARK);
            if (harmful) fx[i].active = 0;
        }
        return 1;
    }

    int free_slot = -1;
    for (int i = 0; i < slots; i++) {
        if (!fx[i].active) {
            if (free_slot < 0) free_slot = i;
            continue;
        }
        if (fx[i].effect_type != (uint8_t)effect->type) continue;
        /* Refresh rather than stack. A weaker duplicate does not shorten the
         * stronger one already running, which is what stops a second Rally Howl
         * at half strength from downgrading the first. */
        if (effect->value >= fx[i].value) {
            fx[i].value = effect->value;
            fx[i].source_id = source_id;
        }
        if (effect->duration > fx[i].duration_remaining)
            fx[i].duration_remaining = effect->duration;
        return 1;
    }

    if (free_slot < 0) return 0;

    NPCEffect* e = &fx[free_slot];
    memset(e, 0, sizeof(*e));
    e->active             = 1;
    e->effect_type        = (uint8_t)effect->type;
    e->buff_stat          = (uint8_t)effect->stat;
    e->value              = effect->value;
    e->duration_remaining = effect->duration > 0.0f ? effect->duration : 0.1f;
    e->tick_rate          = effect->tick_rate;
    e->tick_remaining     = effect->tick_rate;
    e->source_id          = source_id;
    return 1;
}

/* --- Expiry and periodic ticks -------------------------------------------- */

int npc_effects_tick(NPCWorld* world, int slot, NPCEntity* npc, float dt,
                     int* out_dot_damage) {
    if (out_dot_damage) *out_dot_damage = 0;
    if (!npc) return 0;

    NPCEffect* fx = npc_world_effects(world, slot);
    int slots = npc_world_max_effect_slots(world);
    if (!fx || slots <= 0) return 0;

    int total = 0;

    for (int i = 0; i < slots; i++) {
        NPCEffect* e = &fx[i];
        if (!e->active) continue;

        e->duration_remaining -= dt;
        if (e->duration_remaining <= 0.0f) {
            e->active = 0;
            continue;
        }

        if (e->tick_rate <= 0.0f) continue;
        e->tick_remaining -= dt;
        if (e->tick_remaining > 0.0f) continue;
        e->tick_remaining = e->tick_rate;

        if (e->effect_type == EFFECT_DOT) {
            int dmg = e->value > 0 ? e->value : 0;
            if (dmg > 0) {
                npc->health -= dmg;
                total += dmg;
            }
        } else if (e->effect_type == EFFECT_HOT) {
            npc->health += e->value;
            if (npc->health > npc->max_health) npc->health = npc->max_health;
        } else if (e->effect_type == EFFECT_HOT_PERCENT) {
            npc->health += percent_of_max(npc->max_health,
                                          effect_permille_to_fraction(e->value));
            if (npc->health > npc->max_health) npc->health = npc->max_health;
        }
    }

    if (out_dot_damage) *out_dot_damage = total;

    if (npc->health <= 0 && npc->is_alive) {
        npc->health = 0;
        return 1;
    }
    return 0;
}

/* --- Collection ----------------------------------------------------------- */

void npc_collect_modifiers(NPCWorld* world, int slot, const NPCEntity* npc,
                           DamageModifiers* out) {
    if (!out) return;
    damage_mods_reset(out);
    if (!npc) return;

    damage_mods_add_armor(out, npc->armor);

    /* Trigger and affix modifiers. A positive damage_taken_pct in content means
     * "takes more", which is a negative reduction in damage_model.h's terms --
     * one sign convention, stated here, rather than two conventions agreeing by
     * accident everywhere they meet. */
    damage_mods_add_dealt(out, npc->mod_damage_pct / 100.0);
    damage_mods_add_taken(out, -(double)npc->mod_damage_taken_pct / 100.0);

    NPCEffect* fx = npc_world_effects((NPCWorld*)world, slot);
    int slots = npc_world_max_effect_slots(world);
    if (!fx || slots <= 0) return;

    for (int i = 0; i < slots; i++) {
        if (!fx[i].active) continue;
        switch (fx[i].effect_type) {
            case EFFECT_DAMAGE_DEALT:
                damage_mods_add_dealt(out, effect_permille_to_fraction(fx[i].value));
                break;
            case EFFECT_DAMAGE_TAKEN:
                damage_mods_add_taken(out, effect_permille_to_fraction(fx[i].value));
                break;
            case EFFECT_MARK:
                /* A mark raises damage taken, so it is a negative reduction. */
                damage_mods_add_taken(out, -effect_permille_to_fraction(fx[i].value));
                break;
            default:
                break;
        }
    }
}

float npc_speed_multiplier(NPCWorld* world, int slot, const NPCEntity* npc) {
    double pct = npc ? (double)npc->mod_move_speed_pct / 100.0 : 0.0;

    NPCEffect* fx = npc_world_effects((NPCWorld*)world, slot);
    int slots = npc_world_max_effect_slots(world);
    if (fx && slots > 0) {
        for (int i = 0; i < slots; i++) {
            if (!fx[i].active) continue;
            if (fx[i].effect_type == EFFECT_SLOW)
                pct -= (double)fx[i].value / 100.0;
            else if (fx[i].effect_type == EFFECT_BUFF &&
                     fx[i].buff_stat == (uint8_t)STAT_TARGET_MOVE_SPEED)
                pct += (double)fx[i].value / 100.0;
        }
    }

    float m = (float)(1.0 + pct);
    return m < NPC_SPEED_FLOOR ? NPC_SPEED_FLOOR : m;
}

/** Report whether any of a slot's effects is one of two types. */
static int has_any(NPCWorld* world, int slot, int a, int b, int c) {
    NPCEffect* fx = npc_world_effects(world, slot);
    int slots = npc_world_max_effect_slots(world);
    if (!fx || slots <= 0) return 0;
    for (int i = 0; i < slots; i++) {
        if (!fx[i].active) continue;
        int t = fx[i].effect_type;
        if (t == a || t == b || t == c) return 1;
    }
    return 0;
}

int npc_is_action_locked(NPCWorld* world, int slot) {
    return has_any(world, slot, EFFECT_STUN, EFFECT_CHANNEL, EFFECT_KNOCKUP);
}

int npc_is_movement_locked(NPCWorld* world, int slot, int cc_immune) {
    if (cc_immune) return 0;
    return has_any(world, slot, EFFECT_STUN, EFFECT_CHANNEL, EFFECT_ROOT);
}

/* --- Ally queries (V12) --------------------------------------------------- */

/** Bound one ally query. Nearest-first, so truncation drops the furthest. */
#define NPC_ALLY_CANDIDATES 32

/** Test whether a snapshot entry is an ally of the asking NPC. */
static int is_ally(NPCTickSnapshot* npcs, int c, uint32_t self_id, int faction_index) {
    if (npcs->id[c] == self_id) return 0;
    if (npcs->health[c] <= 0) return 0;
    if (npcs->category[c] != NPC_CATEGORY_HOSTILE) return 0;
    if (faction_index < 0) return 1;
    return npc_faction_of_type(npcs->npc_type_id[c]) == faction_index;
}

int npc_allies_near(NPCTickSnapshot* npcs, float x, float y, float radius,
                    uint32_t self_id, int faction_index,
                    int* out_indices, int max_out) {
    if (!npcs || !out_indices || max_out <= 0 || radius <= 0.0f) return 0;

    int scratch[NPC_ALLY_CANDIDATES];
    int found = npc_snapshot_query(npcs, x, y, radius, scratch, NPC_ALLY_CANDIDATES);

    int written = 0;
    for (int k = 0; k < found && written < max_out; k++) {
        int c = scratch[k];
        if (!is_ally(npcs, c, self_id, faction_index)) continue;
        out_indices[written++] = c;
    }
    return written;
}

int npc_allies_below_health(NPCTickSnapshot* npcs, float x, float y, float radius,
                            uint32_t self_id, int faction_index, float fraction) {
    int idx[NPC_ALLY_CANDIDATES];
    int n = npc_allies_near(npcs, x, y, radius, self_id, faction_index,
                            idx, NPC_ALLY_CANDIDATES);
    int count = 0;
    for (int k = 0; k < n; k++) {
        int c = idx[k];
        if (npcs->max_health[c] <= 0) continue;
        float f = (float)npcs->health[c] / (float)npcs->max_health[c];
        if (f <= fraction) count++;
    }
    return count;
}

int npc_lowest_health_ally(NPCTickSnapshot* npcs, float x, float y, float radius,
                           uint32_t self_id, int faction_index) {
    int idx[NPC_ALLY_CANDIDATES];
    int n = npc_allies_near(npcs, x, y, radius, self_id, faction_index,
                            idx, NPC_ALLY_CANDIDATES);
    int best = -1;
    float best_f = 1.0f;
    for (int k = 0; k < n; k++) {
        int c = idx[k];
        if (npcs->max_health[c] <= 0) continue;
        float f = (float)npcs->health[c] / (float)npcs->max_health[c];
        if (f < best_f) { best_f = f; best = c; }
    }
    /* A fully healed ally is not a heal target: Mend on a healthy pack should
     * hold its cooldown rather than spend it on nobody. */
    return best_f >= 0.999f ? -1 : best;
}
