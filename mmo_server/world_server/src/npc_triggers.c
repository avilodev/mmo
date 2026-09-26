/**
 * @file
 * One trigger table, one evaluator, and the phase machine that `set_phase` names.
 *
 * The behaviours this replaces are listed in npc_triggers.h. What is worth saying
 * here is the shape they share: each is "when X, do Y", and the nine ways
 * enemy_types.txt writes X differ only in where the evaluator looks -- health, a
 * clock, a counter, a distance, an ally count. So there is one loop over a type's
 * triggers and a switch with no enemy's name in it.
 *
 * Two of the eight events are counters incremented elsewhere -- incoming damage
 * and kills -- because those happen on threads that are not the gameplay thread
 * and at code sites that hold no profile. Everything else is a predicate this
 * file evaluates from state it already has.
 */

#include "npc_triggers.h"
#include "npc_behavior.h"
#include "npc_effects.h"
#include "player_cast_flags.h"
#include "world_collision.h"
#include "log.h"

#include <math.h>
#include <stdlib.h>
#include <time.h>

/** Read the monotonic clock the NPC's absolute expiries are measured against. */
static double now_monotonic(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

/** Radius within which a player's cast counts as "nearby", in tiles. */
#define TRIGGER_CAST_NEARBY_TILES 6.0f

/** Default seconds a `flee` action lasts when the trigger names no value. */
#define TRIGGER_FLEE_DEFAULT 5.0

/** Walk a type's triggers and then each of its affixes', as one list.
 *
 * The concatenation is what the latch bitset is indexed by, and it is why
 * compute_maxima() sizes that bitset from type triggers *plus* affix triggers:
 * an affix that composes on and whose triggers fall off the end of the bitset
 * would compose on and do nothing.
 */
typedef struct {
    const NPCTypeDef* type;
    int local;        /**< Latch index: position in the concatenated list. */
    int affix;        /**< -1 while walking the type's own triggers. */
    int within;       /**< Position within the current source's list. */
} TriggerCursor;

/** Return the next trigger in the concatenated list, or NULL at the end. */
static const NPCTriggerDef* trigger_next(TriggerCursor* c) {
    const NPCTypeDef* t = c->type;

    if (c->affix < 0) {
        if (c->within < t->trigger_count)
            return npc_registry_trigger(t->trigger_first + c->within++);
        c->affix = 0;
        c->within = 0;
    }

    while (c->affix < t->affix_count) {
        const NPCAffixDef* a = npc_type_affix(t, c->affix);
        if (a && c->within < a->trigger_count)
            return npc_registry_trigger(a->trigger_first + c->within++);
        c->affix++;
        c->within = 0;
    }
    return NULL;
}

/* --- Event predicates ----------------------------------------------------- */

/** Report whether a player within range of this NPC is mid-cast. */
static int player_casting_near(NPCThink* t, float radius) {
    if (!t->players) return 0;
    int nearby[NPC_AGGRO_CANDIDATES];
    int n = tick_snapshot_query(t->players, t->npc->pos_x, t->npc->pos_y, radius,
                                nearby, NPC_AGGRO_CANDIDATES);
    for (int k = 0; k < n; k++) {
        if (t->players->is_dead[nearby[k]]) continue;
        if (player_cast_flag_get(t->players->slot[nearby[k]])) return 1;
    }
    return 0;
}

/** Decide whether one trigger's event holds right now.
 *
 * `on_death` never holds here: it is evaluated by npc_triggers_on_death(), which
 * is the only caller that runs against a corpse.
 */
static int event_holds(NPCThink* t, const NPCTriggerDef* trg) {
    NPCEntity* npc = t->npc;

    switch (trg->when) {
        case NPC_WHEN_HP_BELOW:
            if (npc->max_health <= 0) return 0;
            return (double)npc->health * 100.0 <= (double)npc->max_health * trg->value;

        case NPC_WHEN_TIMER: {
            double period = trg->value > 0.0f ? (double)trg->value : 1.0;
            if (npc->trigger_timer_next <= 0.0) return 0;
            return t->now >= npc->trigger_timer_next && (period > 0.0);
        }

        case NPC_WHEN_INCOMING_DAMAGE:
            return npc->damage_events != npc->damage_events_seen;

        case NPC_WHEN_ABILITY_CAST_NEARBY:
            return player_casting_near(t, TRIGGER_CAST_NEARBY_TILES * world_tile_size());

        case NPC_WHEN_ON_DEATH:
            return 0;

        case NPC_WHEN_PROXIMITY:
            if (!t->has_target) return 0;
            return t->target_dist <= trg->value * world_tile_size();

        case NPC_WHEN_ALLIES_BELOW: {
            if (!t->npcs) return 0;
            int idx[NPC_AGGRO_CANDIDATES];
            int n = npc_allies_near(t->npcs, npc->pos_x, npc->pos_y,
                                    t->prof->aggro_range, npc->id,
                                    t->faction_index, idx, NPC_AGGRO_CANDIDATES);
            return n < (int)trg->value;
        }

        case NPC_WHEN_ON_KILL:
            return npc->kill_events != npc->kill_events_seen;

        case NPC_WHEN_ABILITY_MISSED: {
            /* `value` is how many whiffs it takes, so Armor Break's "two charges
             * without a hit" is the number the content already states. A value
             * of zero or one fires on every miss. */
            uint32_t needed = trg->value > 1.0f ? (uint32_t)trg->value : 1u;
            return npc->miss_events - npc->miss_events_seen >= needed;
        }

        default:
            return 0;
    }
}

/* --- Actions -------------------------------------------------------------- */

/** Apply a `modify` trigger's percentages to this NPC's accumulators. */
static void do_modify(NPCEntity* npc, const NPCTriggerDef* trg) {
    npc->mod_move_speed_pct     += trg->move_speed_pct;
    npc->mod_attack_speed_pct   += trg->attack_speed_pct;
    npc->mod_damage_pct         += trg->damage_pct;
    npc->mod_damage_taken_pct   += trg->damage_taken_pct;
    npc->mod_cooldown_pct       += trg->cooldown_pct;
    npc->mod_phase_duration_pct += trg->phase_duration_pct;

    /* An attack-speed or cooldown modifier that reached -100% would divide by
     * zero in the act phase's cooldown scaling. Clamped here rather than there,
     * so every reader gets a usable number instead of each defending itself. */
    if (npc->mod_attack_speed_pct < -90.0f) npc->mod_attack_speed_pct = -90.0f;
    if (npc->mod_cooldown_pct     < -90.0f) npc->mod_cooldown_pct     = -90.0f;
    if (npc->mod_phase_duration_pct < -90.0f) npc->mod_phase_duration_pct = -90.0f;
}

/** Fire a trigger's ability, aimed at the current target or at the NPC itself. */
static void do_cast(NPCThink* t, const NPCTriggerDef* trg) {
    const NPCAbilityDef* ab = npc_ai_registry_ability(trg->ability_index);
    if (!ab) return;

    float ax = t->has_target ? t->target_x : t->npc->pos_x;
    float ay = t->has_target ? t->target_y : t->npc->pos_y;

    /* A reactive displacement moves *away* from what threatened it, which is the
     * whole of the Rabbit's Evasive Hop: the hop is not a dodge roll, it is not
     * being where the attack lands. Aiming it at the target would hop into the
     * telegraph rather than out of it. */
    if (ab->src->delivery == NPC_DELIV_DISPLACE &&
        (trg->when == NPC_WHEN_INCOMING_DAMAGE || trg->when == NPC_WHEN_PROXIMITY)) {
        float fx = t->npc->pos_x - t->npc->last_damage_x;
        float fy = t->npc->pos_y - t->npc->last_damage_y;
        float len = sqrtf(fx * fx + fy * fy);
        if (len < 0.001f) { fx = 1.0f; fy = 0.0f; len = 1.0f; }
        ax = t->npc->pos_x + (fx / len) * ab->displace_distance;
        ay = t->npc->pos_y + (fy / len) * ab->displace_distance;
    }

    npc_behavior_cast(t, ab, -1, ax, ay);
}

/** Perform one fired trigger. */
static void do_action(NPCThink* t, const NPCTriggerDef* trg) {
    NPCEntity* npc = t->npc;

    switch (trg->action) {
        case NPC_DO_MODIFY:
            do_modify(npc, trg);
            break;

        case NPC_DO_CAST:
        case NPC_DO_SUMMON:
            /* One path, because `summon` is `cast` of an ability whose delivery
             * happens to be a summon. Keeping them apart would mean two ways to
             * spell the same thing and one of them going stale. */
            do_cast(t, trg);
            break;

        case NPC_DO_SWAP_ABILITY:
            if (trg->from_index >= 0 && trg->to_index >= 0)
                npc_world_ability_swap(t->world, t->slot, trg->from_index, trg->to_index);
            break;

        case NPC_DO_FLEE:
            npc->flee_until = t->now +
                (trg->value > 0.0f ? (double)trg->value : TRIGGER_FLEE_DEFAULT);
            break;

        case NPC_DO_SET_PHASE:
            if (trg->phase_index >= 0 && t->world->phase_index) {
                t->world->phase_index[t->slot]      = (uint8_t)trg->phase_index;
                t->world->phase_started_at[t->slot] = t->now;
                t->world->phase_uses[t->slot]       = 0;
            }
            break;

        default:
            break;
    }
}

/* --- Evaluation ----------------------------------------------------------- */

void npc_triggers_evaluate(NPCThink* t) {
    const NPCTypeDef* type = t->prof ? t->prof->type : NULL;
    if (!type || !t->npc) return;

    NPCEntity* npc = t->npc;

    if (npc->trigger_epoch <= 0.0) npc->trigger_epoch = t->now;

    TriggerCursor cur = { type, 0, -1, 0 };
    const NPCTriggerDef* trg;

    while ((trg = trigger_next(&cur)) != NULL) {
        int li = cur.local++;

        /* A timer trigger arms its own next firing instant the first time it is
         * seen, so its period is measured from when this NPC spawned rather than
         * from world start -- otherwise every enemy of a type fires in lockstep. */
        if (trg->when == NPC_WHEN_TIMER && npc->trigger_timer_next <= 0.0)
            npc->trigger_timer_next = npc->trigger_epoch +
                (trg->value > 0.0f ? (double)trg->value : 1.0);

        if (trg->once && npc_world_latch_get(t->world, t->slot, li)) continue;
        if (!event_holds(t, trg)) continue;

        do_action(t, trg);

        if (trg->once) npc_world_latch_set(t->world, t->slot, li);

        if (trg->when == NPC_WHEN_TIMER) {
            double period = trg->value > 0.0f ? (double)trg->value : 1.0;
            npc->trigger_timer_next = trg->repeating ? t->now + period : 0.0;
            if (!trg->repeating) npc_world_latch_set(t->world, t->slot, li);
        }
    }

    /* Consume the event counters after every trigger has had a chance at them:
     * two triggers on one enemy both watching incoming damage must both fire on
     * the same hit, not race for it. */
    npc->damage_events_seen = npc->damage_events;
    npc->kill_events_seen   = npc->kill_events;
    npc->miss_events_seen   = npc->miss_events;
}

void npc_triggers_on_death(NPCThink* t) {
    const NPCTypeDef* type = t->prof ? t->prof->type : NULL;
    if (!type || !t->npc) return;

    TriggerCursor cur = { type, 0, -1, 0 };
    const NPCTriggerDef* trg;

    while ((trg = trigger_next(&cur)) != NULL) {
        int li = cur.local++;
        if (trg->when != NPC_WHEN_ON_DEATH) continue;
        /* Always latched, whatever the content says: a corpse persists for its
         * whole respawn timer, and an unlatched death trigger would summon a new
         * wave every tick until it got back up. */
        if (npc_world_latch_get(t->world, t->slot, li)) continue;
        npc_world_latch_set(t->world, t->slot, li);
        do_action(t, trg);
    }
}

/* --- Phases --------------------------------------------------------------- */

/** Return the phase a slot is in, or NULL when its type declares none. */
static const NPCPhaseDef* current_phase(NPCThink* t) {
    const NPCTypeDef* type = t->prof ? t->prof->type : NULL;
    if (!type || type->phase_count <= 0 || !t->world->phase_index) return NULL;

    int p = t->world->phase_index[t->slot];
    if (p < 0 || p >= type->phase_count) p = 0;
    return npc_registry_phase(type->phase_first + p);
}

int npc_phase_allows(NPCThink* t, int ability_slot) {
    const NPCPhaseDef* ph = current_phase(t);
    if (!ph) return 1;

    for (int i = 0; i < ph->slot_count; i++)
        if (npc_registry_phase_slot(ph->slot_first + i) == ability_slot) return 1;
    return 0;
}

void npc_phase_advance(NPCThink* t) {
    const NPCTypeDef* type = t->prof ? t->prof->type : NULL;
    if (!type || type->phase_count <= 0 || !t->world->phase_index) return;

    const NPCPhaseDef* ph = current_phase(t);
    if (!ph) return;

    if (t->world->phase_started_at[t->slot] <= 0.0)
        t->world->phase_started_at[t->slot] = t->now;

    if (t->ability_used && t->world->phase_uses[t->slot] < 0xFFFF)
        t->world->phase_uses[t->slot]++;

    int exit_now = 0;
    switch (ph->exit_kind) {
        case NPC_PHASE_AFTER: {
            /* Phase Break shortens every phase this NPC will ever enter, which is
             * exactly what "the cycle speeds up" means and why the modifier lives
             * on the NPC rather than on one phase. */
            double window = (double)ph->exit_value *
                            (1.0 + (double)t->npc->mod_phase_duration_pct / 100.0);
            if (window < 0.2) window = 0.2;
            exit_now = (t->now - t->world->phase_started_at[t->slot]) >= window;
            break;
        }
        case NPC_PHASE_AFTER_USES:
            exit_now = t->world->phase_uses[t->slot] >= (uint16_t)ph->exit_value;
            break;
        case NPC_PHASE_TARGET_WITHIN:
            exit_now = t->has_target &&
                       t->target_dist <= ph->exit_value * world_tile_size();
            break;
        case NPC_PHASE_TARGET_BEYOND:
            exit_now = t->has_target &&
                       t->target_dist >= ph->exit_value * world_tile_size();
            break;
        default:
            break;
    }

    if (!exit_now) return;

    int next = (t->world->phase_index[t->slot] + 1) % type->phase_count;
    t->world->phase_index[t->slot]      = (uint8_t)next;
    t->world->phase_started_at[t->slot] = t->now;
    t->world->phase_uses[t->slot]       = 0;
}

/* --- Event sources -------------------------------------------------------- */

void npc_trigger_note_damage(NPCEntity* npc, float from_x, float from_y) {
    if (!npc) return;
    npc->damage_events++;
    npc->last_damage_x = from_x;
    npc->last_damage_y = from_y;

    /* Being hit gives a stealthed NPC away. This is the hook because it is
     * already called from every one of the four places NPC health is written,
     * which is exactly the set of places stealth has to break. */
    npc_stealth_reveal(npc, now_monotonic());
}

void npc_trigger_note_kill(NPCEntity* npc) {
    if (npc) npc->kill_events++;
}
