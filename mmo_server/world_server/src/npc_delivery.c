/**
 * @file
 * Turn a chosen ability into queued work: the delivery half of the act phase.
 *
 * Split from npc_behavior.c at the seam that already existed between deciding
 * *which* ability and describing *what it does*, when the two together crossed
 * the project's 800-line ceiling. The decision half reads the world; this half
 * only writes the queue, which is why the two divide cleanly.
 *
 * Nothing here performs an ability. Every delivery -- a projectile, a zone, a
 * summon, damage against a player -- reaches state this NPC's slot lock does not
 * cover, so it is described into npc_deferred.h and performed once every lock is
 * released. The exceptions are this NPC's own position, cooldowns and cast state,
 * which its lock does cover and which are written here directly.
 *
 * Three verbs live here in full: V2 multi-shot, which walks its shots across
 * ticks rather than looping so a player can react between them; V8 displacement,
 * which travels against real collision rather than teleporting; and V14's
 * telegraph variants, which are three different meanings for one wire packet.
 */

#include "npc_delivery.h"
#include "npc_behavior.h"
#include "npc_effects.h"
#include "world_collision.h"
#include "log.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/** Aim jitter applied to every ability, in degrees either way. */
#define NPC_AIM_JITTER_DEG 8

/** Return a uniform sample in [lo, hi]. */
static float frand_range(float lo, float hi) {
    return lo + (hi - lo) * ((float)(rand() % 1001) / 1000.0f);
}

/** Rotate a direction by an angle in degrees. */
static void rotate(float* x, float* y, float degrees) {
    float r = degrees * (float)(M_PI / 180.0);
    float c = cosf(r), s = sinf(r);
    float nx = *x * c - *y * s;
    float ny = *x * s + *y * c;
    *x = nx; *y = ny;
}

/* --- Modifier-aware ability numbers --------------------------------------
 *
 * Three scalars change per instance: how often an ability may fire, how long it
 * telegraphs, and how hard it lands. Each is read through one function so that a
 * Frenzy, an affix and a Phase Break compose the same way everywhere.
 */

/** Seconds this NPC must wait between uses of an ability. */
double npc_ability_cooldown(const NPCEntity* npc, const NPCAbilityDef* ab) {
    double cd = ab->src->cooldown;
    cd *= 1.0 + (double)npc->mod_cooldown_pct / 100.0;
    cd /= 1.0 + (double)npc->mod_attack_speed_pct / 100.0;
    return cd < 0.05 ? 0.05 : cd;
}

/** Seconds this NPC telegraphs an ability for. */
double npc_ability_cast_time(const NPCEntity* npc, const NPCAbilityDef* ab) {
    double ct = ab->src->cast_time;
    ct *= 1.0 + (double)npc->mod_cast_time_pct / 100.0;
    return ct < 0.0 ? 0.0 : ct;
}

/** Damage this NPC's ability lands for, before the target's mitigation. */
int npc_ability_damage(const NPCEntity* npc, const NPCAbilityDef* ab) {
    double d = (double)ab->src->damage * (1.0 + (double)npc->mod_damage_pct / 100.0);
    return d < 0.0 ? 0 : (int)(d + 0.5);
}

/* --- Deferred description ------------------------------------------------- */

/** Fill the fields every queued action carries, whatever its kind. */
void npc_delivery_action_begin(const NPCThink* t, NPCDeferredAction* d, NPCDeferredType kind) {
    memset(d, 0, sizeof(*d));
    d->type          = (uint8_t)kind;
    d->npc_id        = t->npc->id;
    d->npc_slot      = t->slot;
    d->npc_x         = t->npc->pos_x;
    d->npc_y         = t->npc->pos_y;
    d->npc_type_id   = t->npc->npc_type_id;
    d->faction_index = t->faction_index;
}

/** Queue the optional components an ability carries alongside its delivery.
 *
 * Zones, buffs, heals and summons are components rather than deliveries: an
 * ability may be a telegraph *and* leave a puddle, which is one attack a player
 * sees once. They fire when the ability resolves -- at once for an instant
 * delivery, at cast completion for a telegraph.
 */
void npc_delivery_components(NPCThink* t, const NPCAbilityDef* ab,
                               float at_x, float at_y) {
    const NPCAbilityDefn* s = ab->src;
    NPCDeferredAction d;

    if (s->has_zone_impact && s->zone_impact.duration > 0.0f) {
        npc_delivery_action_begin(t, &d, NPC_ACT_ZONE);
        d.zone.ability_id   = ab->ability_id;
        d.zone.pos_x        = at_x;
        d.zone.pos_y        = at_y;
        d.zone.radius       = ab->zone_impact_radius;
        d.zone.duration     = s->zone_impact.duration;
        d.zone.tick_rate    = s->zone_impact.tick_rate;
        d.zone.count        = s->zone_impact.count > 0 ? s->zone_impact.count : 1;
        d.zone.spread       = ab->zone_impact_radius * 2.0f;
        d.zone.effect_first = s->zone_impact.effect_first;
        d.zone.effect_count = s->zone_impact.effect_count;
        npc_deferred_push(t->q, &d);
    }

    if (s->has_zone_self && s->zone_self.duration > 0.0f) {
        npc_delivery_action_begin(t, &d, NPC_ACT_ZONE);
        d.zone.ability_id   = ab->ability_id;
        d.zone.pos_x        = t->npc->pos_x;
        d.zone.pos_y        = t->npc->pos_y;
        d.zone.radius       = ab->zone_self_radius;
        d.zone.duration     = s->zone_self.duration;
        d.zone.tick_rate    = s->zone_self.tick_rate;
        d.zone.count        = s->zone_self.count > 0 ? s->zone_self.count : 1;
        d.zone.spread       = ab->zone_self_radius * 2.0f;
        d.zone.effect_first = s->zone_self.effect_first;
        d.zone.effect_count = s->zone_self.effect_count;
        npc_deferred_push(t->q, &d);
    }

    if (s->has_buff && (s->buff.effect_count > 0 || s->buff.ready_allies ||
                        s->buff.retarget_allies)) {
        npc_delivery_action_begin(t, &d, NPC_ACT_BUFF);
        d.buff.ability_id      = ab->ability_id;
        d.buff.radius          = ab->buff_radius;
        d.buff.duration        = s->buff.duration;
        d.buff.target_self     = s->buff.target_self;
        d.buff.faction_only    = s->buff.faction_only;
        d.buff.single_ally     = s->buff.single_ally;
        d.buff.ready_allies    = s->buff.ready_allies;
        d.buff.retarget_allies = s->buff.retarget_allies;
        d.buff.target_id       = t->target_id;
        d.buff.effect_first    = s->buff.effect_first;
        d.buff.effect_count    = s->buff.effect_count;
        npc_deferred_push(t->q, &d);
    }

    if (s->has_heal && (s->heal.amount > 0 || s->heal.percent > 0)) {
        npc_delivery_action_begin(t, &d, NPC_ACT_HEAL);
        d.heal.ability_id       = ab->ability_id;
        d.heal.radius           = ab->heal_radius;
        d.heal.amount           = s->heal.amount;
        d.heal.percent          = s->heal.percent;
        d.heal.lowest_ally_only = s->heal.lowest_ally_only;
        d.heal.target_self      = (uint8_t)(ab->heal_radius <= 0.0f);
        npc_deferred_push(t->q, &d);
    }

    if (s->has_summon && s->summon.type_count > 0) {
        npc_delivery_action_begin(t, &d, NPC_ACT_SUMMON);
        d.summon.ability_id = ab->ability_id;
        d.summon.type_count = s->summon.type_count;
        for (int i = 0; i < s->summon.type_count && i < 4; i++)
            d.summon.type_index[i] = s->summon.type_index[i];
        d.summon.count_min = s->summon.count_min;
        d.summon.count_max = s->summon.count_max;
        d.summon.spread    = ab->summon_spread;
        npc_deferred_push(t->q, &d);
    }
}

/** Queue one projectile along a direction. */
static void queue_projectile(NPCThink* t, const NPCAbilityDef* ab,
                             float dir_x, float dir_y) {
    NPCDeferredAction d;
    npc_delivery_action_begin(t, &d, NPC_ACT_PROJECTILE);
    d.proj.ability_id   = ab->ability_id;
    d.proj.origin_x     = t->npc->pos_x;
    d.proj.origin_y     = t->npc->pos_y;
    d.proj.target_x     = t->npc->pos_x + dir_x * ab->range;
    d.proj.target_y     = t->npc->pos_y + dir_y * ab->range;
    d.proj.damage       = npc_ability_damage(t->npc, ab);
    d.proj.damage_type  = ab->src->damage_type;
    d.proj.speed        = ab->src->projectile.speed;
    d.proj.width        = ab->src->projectile.width;
    d.proj.range        = ab->range;
    d.proj.effect_first = ab->src->effect_first;
    d.proj.effect_count = ab->src->effect_count;
    npc_deferred_push(t->q, &d);
}

/** Fire one shot of a burst, fanned by its position within the volley (V2). */
static void fire_burst_shot(NPCThink* t, const NPCAbilityDef* ab,
                            float dir_x, float dir_y, int index, int count) {
    float spread = ab->src->burst.spread_angle;
    if (count > 1 && spread > 0.0f) {
        /* Centre the fan on the aim direction: three shots at 30 degrees are
         * -15, 0, +15, not 0, 30, 60 -- one of those is aimed at the player. */
        float step = spread / (float)(count - 1);
        rotate(&dir_x, &dir_y, -spread * 0.5f + step * (float)index);
    }
    queue_projectile(t, ab, dir_x, dir_y);
}

/* --- Deliveries ----------------------------------------------------------- */

/** Move this NPC as an ability says to (V8).
 *
 * Four modes, one function. A teleport arrives now and only has to be a legal
 * place to stand; a dash and a hop travel and are advanced by the tick, so both
 * can be walked into by a player and both stop on terrain. An ally swap is a
 * teleport whose destination is another NPC -- Ashen Acolyte trading places with
 * a decoy -- which is a destination, not a mechanic of its own.
 */
static void apply_displacement(NPCThink* t, const NPCAbilityDef* ab,
                               float dir_x, float dir_y) {
    NPCEntity* npc = t->npc;
    const NPCDisplaceSpec* dsp = &ab->src->displace;

    float travel = ab->displace_distance;
    if (travel <= 0.0f) travel = world_tile_size();

    float dest_x = npc->pos_x + dir_x * travel;
    float dest_y = npc->pos_y + dir_y * travel;

    if (dsp->behind_target && t->has_target) {
        /* Arrive on the far side of the target, which is what "flank" means and
         * what makes a Wraith worth turning around for. */
        float bx = t->target_x - npc->pos_x, by = t->target_y - npc->pos_y;
        float bl = sqrtf(bx * bx + by * by);
        if (bl > 0.001f) {
            dest_x = t->target_x + (bx / bl) * world_tile_size();
            dest_y = t->target_y + (by / bl) * world_tile_size();
        }
    } else if (dsp->mode == 3) {
        int idx[8];
        int n = t->npcs ? npc_allies_near(t->npcs, npc->pos_x, npc->pos_y,
                                          travel, npc->id, t->faction_index,
                                          idx, 8) : 0;
        /* No ally to trade with means no swap. Teleporting to nowhere in
         * particular would be a different ability than the one authored. */
        if (n == 0) return;
        dest_x = t->npcs->pos_x[idx[0]];
        dest_y = t->npcs->pos_y[idx[0]];
    }

    if (dsp->mode == 1 || dsp->mode == 3) {
        if (!world_collision_check_box(dest_x, dest_y, npc->hitbox_radius) &&
            world_coord_is_valid(dest_x, dest_y)) {
            npc->pos_x = dest_x;
            npc->pos_y = dest_y;
        }
        return;
    }

    float dx = dest_x - npc->pos_x, dy = dest_y - npc->pos_y;
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 0.001f) return;

    npc->dash_active    = 1;
    npc->dash_dir_x     = dx / len;
    npc->dash_dir_y     = dy / len;
    npc->dash_remaining = len;
    /* A hop is a short escape and a dash is a commitment, so they cover the same
     * ground at different speeds. */
    npc->dash_speed     = (dsp->mode == 2) ? len / 0.25f
                                           : (t->prof->move_speed * 3.5f + 200.0f);
    npc->dash_damage    = dsp->damage_on_contact ? npc_ability_damage(npc, ab) : 0;
    npc->dash_ability_id = ab->ability_id;
    npc->dash_stagger    = dsp->terrain_stagger;
}

void npc_behavior_cast(NPCThink* t, const NPCAbilityDef* ab, int ability_slot,
                       float aim_x, float aim_y) {
    if (!ab || !ab->src) return;
    NPCEntity* npc = t->npc;
    const NPCAbilityDefn* s = ab->src;

    float dx = aim_x - npc->pos_x;
    float dy = aim_y - npc->pos_y;
    float len = sqrtf(dx * dx + dy * dy);
    float dir_x = (len > 0.001f) ? dx / len : (npc->facing_x != 0.0f ? npc->facing_x : 1.0f);
    float dir_y = (len > 0.001f) ? dy / len : npc->facing_y;

    /* Aim jitter, so a pack does not fire a single perfectly overlapping line. */
    rotate(&dir_x, &dir_y, (float)(rand() % (NPC_AIM_JITTER_DEG * 2 + 1) - NPC_AIM_JITTER_DEG));

    npc->facing_x = dir_x;
    npc->facing_y = dir_y;

    /* Acting gives a stealthed NPC away: the ambush is the reveal. */
    npc_stealth_reveal(npc, t->now);

    /* Start this ability's cooldown at the moment it is *committed*, not when it
     * resolves: a two-second telegraph would otherwise be free for its whole
     * cast. Variance of +/-20% keeps a pack off a shared cadence. */
    if (ability_slot >= 0 && ability_slot < t->cooldown_count && t->cooldowns) {
        float v = (float)(rand() % 40 - 20) / 100.0f;
        t->cooldowns[ability_slot] = t->now - npc_ability_cooldown(npc, ab) * v;
    }
    t->ability_used = 1;

    if (s->recovery > 0.0f)
        npc->recover_until = t->now + s->recovery;

    /* An ability that costs health to cast pays before it lands, which is what
     * makes Blood Ritualist a race the player can win by pressuring it. */
    if (s->self_cost_health_percent > 0 && npc->max_health > 0) {
        int cost = npc->max_health * s->self_cost_health_percent / 100;
        npc->health -= cost > 0 ? cost : 1;
        if (npc->health < 1) npc->health = 1;
    }

    /* A displacement carried alongside another delivery moves first: an ambush
     * that struck from where it started and then teleported would be a teleport
     * with a hit attached, not an ambush. */
    if (s->has_displace && s->delivery != NPC_DELIV_DISPLACE)
        apply_displacement(t, ab, dir_x, dir_y);

    switch (s->delivery) {
        case NPC_DELIV_CONTACT: {
            NPCDeferredAction d;
            npc_delivery_action_begin(t, &d, NPC_ACT_CONTACT);
            d.contact.ability_id   = ab->ability_id;
            d.contact.damage       = npc_ability_damage(npc, ab);
            d.contact.damage_type  = s->damage_type;
            d.contact.reach        = npc->hitbox_radius + ab->range;
            d.contact.effect_first = s->effect_first;
            d.contact.effect_count = s->effect_count;
            npc_deferred_push(t->q, &d);
            npc_delivery_components(t, ab, npc->pos_x, npc->pos_y);
            break;
        }

        case NPC_DELIV_PROJECTILE: {
            int shots = (s->has_burst && s->burst.count > 1) ? s->burst.count : 1;

            if (shots > 1 && s->burst.interval > 0.0f) {
                /* A walking burst is committed state, not a loop: the shots have
                 * to leave on separate ticks or the player sees one packet with
                 * three arrows in it and cannot react between them. */
                npc->burst_ability_idx = ability_slot;
                npc->burst_remaining   = shots;
                npc->burst_index       = 0;
                npc->burst_next_time   = t->now;
                npc->burst_dir_x       = dir_x;
                npc->burst_dir_y       = dir_y;
                npc->burst_target_id   = t->target_id;
                break;
            }

            for (int i = 0; i < shots; i++)
                fire_burst_shot(t, ab, dir_x, dir_y, i, shots);
            npc_delivery_components(t, ab, aim_x, aim_y);
            break;
        }

        case NPC_DELIV_TELEGRAPH: {
            float tele_x = s->telegraph.at_target ? aim_x : npc->pos_x;
            float tele_y = s->telegraph.at_target ? aim_y : npc->pos_y;

            double cast_time = npc_ability_cast_time(npc, ab);
            float  reach = s->telegraph.shape == NPC_TELE_CIRCLE ||
                           s->telegraph.shape == NPC_TELE_CONE
                         ? ab->telegraph_radius : ab->telegraph_length;

            /* V14. One animation, more than one meaning: a feint that resolves
             * into nothing, a reach the player cannot read off the wind-up, or a
             * cast time they cannot count. All three are the same telegraph on
             * the wire, which is the point -- they are unreadable by design and
             * fair because the warning is still shown. */
            switch (s->telegraph.resolve_mode) {
                case NPC_RESOLVE_RANDOM_RANGE:
                    reach *= frand_range(0.55f, 1.0f);
                    break;
                case NPC_RESOLVE_RANDOM_CAST_TIME:
                    cast_time *= (double)frand_range(0.6f, 1.5f);
                    break;
                default:
                    break;
            }

            npc->ai_state           = NPC_AI_CASTING;
            npc->ai_is_casting      = 1;
            npc->ai_cast_ability_idx = ability_slot;
            npc->ai_cast_start      = t->now;
            npc->ai_cast_duration   = cast_time;
            npc->ai_cast_pos_x      = tele_x;
            npc->ai_cast_pos_y      = tele_y;
            npc->ai_cast_dir_x      = dir_x;
            npc->ai_cast_dir_y      = dir_y;
            npc->ai_cast_reach      = reach;
            npc->ai_cast_resolves   =
                (uint8_t)(s->telegraph.resolve_mode != NPC_RESOLVE_NONE);

            NPCDeferredAction d;
            npc_delivery_action_begin(t, &d, NPC_ACT_TELEGRAPH_START);
            d.tstart.ability_id = ab->ability_id;
            d.tstart.shape      = s->telegraph.shape;
            d.tstart.pos_x      = tele_x;
            d.tstart.pos_y      = tele_y;
            d.tstart.dir_x      = dir_x;
            d.tstart.dir_y      = dir_y;
            d.tstart.radius     = (s->telegraph.shape == NPC_TELE_CIRCLE ||
                                   s->telegraph.shape == NPC_TELE_CONE)
                                ? reach : ab->telegraph_radius;
            d.tstart.angle      = s->telegraph.angle;
            d.tstart.width      = ab->telegraph_width;
            d.tstart.length     = (s->telegraph.shape == NPC_TELE_RECTANGLE ||
                                   s->telegraph.shape == NPC_TELE_LINE)
                                ? reach : ab->telegraph_length;
            d.tstart.cast_time  = (float)cast_time;
            npc_deferred_push(t->q, &d);
            break;
        }

        case NPC_DELIV_ZONE:
            npc_delivery_components(t, ab, aim_x, aim_y);
            break;

        case NPC_DELIV_DISPLACE:
            apply_displacement(t, ab, dir_x, dir_y);
            npc_delivery_components(t, ab, npc->pos_x, npc->pos_y);
            break;

        case NPC_DELIV_SUMMON:
        case NPC_DELIV_BUFF:
            npc_delivery_components(t, ab, aim_x, aim_y);
            break;

        case NPC_DELIV_PASSIVE:
        default:
            break;
    }
}

/* --- Commitments ---------------------------------------------------------- */

/** Advance a travelling dash by one tick, stopping on terrain. */
static void advance_dash(NPCThink* t) {
    NPCEntity* npc = t->npc;
    float step = npc->dash_speed * t->dt;
    if (step > npc->dash_remaining) step = npc->dash_remaining;

    float nx = npc->pos_x + npc->dash_dir_x * step;
    float ny = npc->pos_y + npc->dash_dir_y * step;

    if (world_collision_check_box_path(npc->pos_x, npc->pos_y, nx, ny,
                                       npc->hitbox_radius) ||
        !world_coord_is_valid(nx, ny)) {
        /* Wall Slam: the charge that hits terrain staggers its own caster, which
         * is the counterplay the source document asks for -- step aside and the
         * Wagon Breaker punishes itself. */
        if (npc->dash_stagger)
            npc->recover_until = t->now + 1.5;
        npc->dash_active = 0;
        npc->dash_remaining = 0.0f;
        return;
    }

    npc->pos_x = nx;
    npc->pos_y = ny;
    npc->dash_remaining -= step;

    if (npc->dash_damage > 0) {
        NPCDeferredAction d;
        npc_delivery_action_begin(t, &d, NPC_ACT_CONTACT);
        d.contact.ability_id  = npc->dash_ability_id;
        d.contact.damage      = npc->dash_damage;
        d.contact.damage_type = ABILITY_DMG_PHYSICAL;
        d.contact.reach       = npc->hitbox_radius + world_tile_size() * 0.5f;
        npc_deferred_push(t->q, &d);
    }

    if (npc->dash_remaining <= 0.001f) npc->dash_active = 0;
}

/** Fire the next shot of a walking burst when its interval has elapsed. */
static void advance_burst(NPCThink* t) {
    NPCEntity* npc = t->npc;
    if (t->now < npc->burst_next_time) return;

    int slot = npc->burst_ability_idx;
    const NPCAbilityDef* ab = (slot >= 0 && slot < t->prof->ability_count)
                            ? &t->prof->abilities[slot] : NULL;
    if (!ab || !ab->src) { npc->burst_remaining = 0; return; }

    int total = ab->src->burst.count > 1 ? ab->src->burst.count : 1;
    fire_burst_shot(t, ab, npc->burst_dir_x, npc->burst_dir_y,
                    npc->burst_index, total);

    npc->burst_index++;
    npc->burst_remaining--;
    npc->burst_next_time = t->now + ab->src->burst.interval;

    if (npc->burst_remaining <= 0)
        npc_delivery_components(t, ab, npc->pos_x + npc->burst_dir_x * ab->range,
                                  npc->pos_y + npc->burst_dir_y * ab->range);
}

int npc_behavior_advance_commitments(NPCThink* t) {
    NPCEntity* npc = t->npc;
    int committed = 0;

    if (npc->dash_active) { advance_dash(t); committed = 1; }
    if (npc->burst_remaining > 0) { advance_burst(t); committed = 1; }

    return committed;
}
