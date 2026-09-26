/**
 * @file
 * One damage gate for shields, frontal blocks and positional weaknesses.
 *
 * Called from each of the four places NPC health is written. That it is four
 * places and one function is the design: a gate that some damage paths consult
 * and others do not is a shield the player can bypass by picking a different
 * ability, which is indistinguishable from a bug and impossible to explain.
 */

#include "npc_mitigation.h"
#include "npc_registry.h"
#include "npc_geometry.h"
#include "npc_effects.h"
#include "world_collision.h"
#include "log.h"

#include <math.h>

void npc_mitigation_init(NPCEntity* npc, uint16_t npc_type_id) {
    if (!npc) return;

    npc->shield_hp          = 0;
    npc->shield_broken      = 0;
    npc->shield_down_until  = 0.0;
    npc->shield_window_hits = 0;
    npc->shield_window_start = 0.0;

    const NPCTypeDef* t = npc_type_get(npc_type_id);
    if (!t || !t->has_mitigation || !t->mitigation.has_shield) return;

    if (t->mitigation.break_mode == NPC_BREAK_SHIELD_POOL)
        npc->shield_hp = t->mitigation.break_pool_hp;
}

int npc_mitigation_shield_up(const NPCEntity* npc) {
    if (!npc) return 0;
    const NPCTypeDef* t = npc_type_get(npc->npc_type_id);
    if (!t || !t->has_mitigation || !t->mitigation.has_shield) return 0;
    return !npc->shield_broken;
}

/** Apply a percentage modifier stated as "damage taken", clamped at zero.
 *
 * Content states these the way a designer reads them: -60 means "takes 40% of
 * it". One conversion, here, rather than a sign convention every caller has to
 * remember.
 */
static int scale_by_pct(int damage, float taken_pct) {
    double f = 1.0 + (double)taken_pct / 100.0;
    if (f < 0.0) f = 0.0;
    int out = (int)((double)damage * f + 0.5);
    return out < 0 ? 0 : out;
}

/** Update a shield's break bookkeeping for one incoming hit.
 *
 * The hit that breaks a shield is still absorbed by it. That is the difference
 * between "four hits break it" meaning four absorbed and then it is gone, and
 * meaning three absorbed and the fourth landing whole -- and it is the reading a
 * player will hold, because they counted four hits and saw the shield shatter on
 * the fourth.
 *
 * @param out_break  Set when this hit was the last one the shield survives.
 * @return           Nonzero when this hit is absorbed.
 */
static int shield_absorbs(const NPCMitigationDef* m, NPCEntity* npc,
                          int damage, double now, int* out_break) {
    *out_break = 0;

    switch (m->break_mode) {
        case NPC_BREAK_HITS_IN_WINDOW:
            /* A counting window: N hits inside `window` seconds break it, and
             * falling short of the pace resets. This is the "burst it down"
             * answer -- chipping at it forever never gets there. */
            if (npc->shield_window_start <= 0.0 ||
                now - npc->shield_window_start > m->break_window) {
                npc->shield_window_start = now;
                npc->shield_window_hits  = 0;
            }
            npc->shield_window_hits++;
            if (npc->shield_window_hits >= (m->break_hits > 0 ? m->break_hits : 1))
                *out_break = 1;
            return 1;

        case NPC_BREAK_SHIELD_POOL:
            /* A pool: the shield eats damage until it is spent. The hit that
             * empties it is absorbed by what was left; the next one is not. */
            npc->shield_hp -= damage;
            if (npc->shield_hp <= 0) {
                npc->shield_hp = 0;
                *out_break = 1;
            }
            return 1;

        case NPC_BREAK_TIMED:
            /* Holds for `window` seconds from the first hit that engages it, then
             * drops on its own. The answer is sustained pressure rather than a
             * single burst -- nothing about how hard you hit it matters. */
            if (npc->shield_window_start <= 0.0) npc->shield_window_start = now;
            if (now - npc->shield_window_start >= m->break_window) *out_break = 1;
            return 1;

        case NPC_BREAK_PARRY_FRAME:
        case NPC_BREAK_EFFECT:
        case NPC_BREAK_NONE:
        default:
            /* A parry frame is a window the shield is *not* up in, which the
             * caster's own cast state expresses; there is nothing to count here.
             * An effect-broken shield is broken by npc_effect_apply, not by a
             * hit. Both stay up until something else takes them down. */
            return 1;
    }
}

int npc_mitigation_apply(NPCWorld* world, int slot, NPCEntity* npc,
                         int damage, float from_x, float from_y,
                         double now, int* out_blocked) {
    (void)world; (void)slot;
    if (out_blocked) *out_blocked = 0;
    if (!npc || damage <= 0) return damage < 0 ? 0 : damage;

    /* A stealthed type may declare itself untargetable while hidden, which is
     * how a Snake in ambush differs from one merely standing in tall grass. */
    if (!npc_stealth_damageable(npc, now)) {
        if (out_blocked) *out_blocked = 1;
        return 0;
    }

    const NPCTypeDef* t = npc_type_get(npc->npc_type_id);
    if (!t || !t->has_mitigation) return damage;
    const NPCMitigationDef* m = &t->mitigation;

    int blocked = 0;

    /* --- Positional -----------------------------------------------------
     *
     * A weak point: inside this radius of the NPC's back, damage lands harder.
     * Checked first because it is a bonus to the attacker and should not be
     * eaten by a shield's reduction being applied to an already-scaled number. */
    if (m->has_positional) {
        float bx = npc->pos_x - npc->facing_x * m->positional_radius_tiles * world_tile_size();
        float by = npc->pos_y - npc->facing_y * m->positional_radius_tiles * world_tile_size();
        float dx = from_x - bx, dy = from_y - by;
        if (sqrtf(dx * dx + dy * dy) <= m->positional_radius_tiles * world_tile_size()) {
            damage = scale_by_pct(damage, m->positional_damage_taken_pct);
            blocked = 1;
        }
    }

    /* --- Frontal block --------------------------------------------------
     *
     * "Not from there." An arc measured against the NPC's facing, so walking
     * around it is the answer -- and an NPC that has never faced anything has a
     * zero-length facing, which is treated as no block rather than as a block in
     * every direction. */
    if (m->has_frontal_block &&
        (npc->facing_x != 0.0f || npc->facing_y != 0.0f)) {
        if (point_in_cone(from_x, from_y, npc->pos_x, npc->pos_y,
                          npc->facing_x, npc->facing_y,
                          1e9f, m->frontal_arc_degrees)) {
            damage = scale_by_pct(damage, m->blocked_damage_pct - 100.0f);
            blocked = 1;
        }
    }

    /* --- Shield ---------------------------------------------------------- */
    if (m->has_shield) {
        if (npc->shield_broken) {
            /* A shield that recharges comes back after its downtime; one that
             * never does stays down for the rest of this NPC's life. */
            if (!m->recharge_never && m->down_seconds > 0.0f &&
                npc->shield_down_until > 0.0 && now >= npc->shield_down_until) {
                npc->shield_broken       = 0;
                npc->shield_window_hits  = 0;
                npc->shield_window_start = 0.0;
                if (m->break_mode == NPC_BREAK_SHIELD_POOL)
                    npc->shield_hp = m->break_pool_hp;
            }
        }

        if (npc->shield_broken) {
            damage = scale_by_pct(damage, m->broken_damage_taken_pct);
            if (m->broken_damage_taken_pct != 0.0f) blocked = 1;
        } else {
            int breaks = 0;
            if (shield_absorbs(m, npc, damage, now, &breaks)) {
                damage = scale_by_pct(damage, m->shield_damage_taken_pct);
                blocked = 1;
            }
            if (breaks) {
                npc->shield_broken     = 1;
                npc->shield_down_until = now + (m->down_seconds > 0.0f
                                              ? m->down_seconds : 0.0f);
                LOG_DEBUG("[MITIGATION] NPC %u's shield broke", npc->id);
            }
        }
    }

    if (out_blocked) *out_blocked = blocked;
    return damage < 0 ? 0 : damage;
}
