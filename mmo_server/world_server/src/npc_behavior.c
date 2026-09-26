/**
 * @file
 * One NPC's think and act phases: target, move, choose, deliver.
 *
 * This was the body of npc_ai_tick(). It moved out when the verbs landed, because
 * five of them are decisions taken here and the function would otherwise have
 * grown past every ceiling in the project while holding a lock.
 *
 * The invariant that shapes the whole file: **the act phase decides, it never
 * performs.** Everything an ability does reaches something other than the NPC
 * whose slot lock is held -- a player's health, an ally's buff slots, the zone
 * pool, the NPC pool's write lock for a summon -- so every one of them is
 * described into the deferred queue and performed after the locks come off. The
 * one exception is this NPC's own position and cooldowns, which the lock covers.
 */

#include "npc_behavior.h"
#include "npc_delivery.h"
#include "npc_effects.h"
#include "npc_geometry.h"
#include "npc_triggers.h"
#include "world_collision.h"
#include "log.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/** Retain aggro past the acquisition range, so a player cannot walk out of a fight. */
#define NPC_HOLD_AGGRO_RANGE 250.0f

/** How close a returning NPC has to get before it counts as home. */
#define NPC_RETURN_SNAP 5.0f

/** Tolerance band around a kiter's preferred range, in world units. */
#define NPC_RANGE_TOLERANCE 30.0f

static float dist2d(float ax, float ay, float bx, float by) {
    float dx = bx - ax, dy = by - ay;
    return sqrtf(dx * dx + dy * dy);
}

/** Return a uniform sample in [lo, hi]. */
static float frand_range(float lo, float hi) {
    return lo + (hi - lo) * ((float)(rand() % 1001) / 1000.0f);
}

/* --- Target selection (V13) ----------------------------------------------- */

/** Score one candidate under an archetype's target priority. Higher wins. */
static float priority_score(NPCThink* t, int dense, float distance) {
    TickSnapshot* s = t->players;

    switch (t->prof->target_priority) {
        case NPC_PRIORITY_BLESSED_FIRST:
            /* A Purge Rusher exists to punish the transformed. Distance still
             * breaks ties, so it does not sprint across a zone past two humans
             * for a Blessed it cannot reach. */
            return (s->form[dense] == FORM_ANIMAL ? 10000.0f : 0.0f) - distance;

        case NPC_PRIORITY_LOWEST_HP: {
            if (s->max_health[dense] <= 0) return -distance;
            float frac = (float)s->health[dense] / (float)s->max_health[dense];
            return (1.0f - frac) * 10000.0f - distance;
        }

        case NPC_PRIORITY_CLUSTER: {
            /* Whoever has the most company. The Purifier's whole design is that
             * standing together is the mistake. */
            int near[NPC_AGGRO_CANDIDATES];
            int n = tick_snapshot_query(s, s->pos_x[dense], s->pos_y[dense],
                                        world_tile_size() * 3.0f,
                                        near, NPC_AGGRO_CANDIDATES);
            return (float)n * 1000.0f - distance;
        }

        case NPC_PRIORITY_NEAREST:
        default:
            return -distance;
    }
}

/** Resolve who this NPC is fighting, filling the context's target fields. */
static void select_target(NPCThink* t) {
    NPCEntity* npc = t->npc;
    TickSnapshot* s = t->players;

    t->has_target   = 0;
    t->target_dense = -1;
    t->target_id    = 0;
    t->target_dist  = 0.0f;

    /* A charm redirects this NPC at whoever charmed it, ahead of everything --
     * including a taunt, because the charm is the more recent instruction and
     * the shorter one. */
    if (npc->charm_expires_at > 0.0) {
        if (t->now >= npc->charm_expires_at) {
            npc->charm_expires_at = 0.0;
            npc->charm_source_id  = 0;
        }
    }

    /* A live taunt overrides target selection outright. It is checked before
     * priority so that a tank pulling a pack off a healer works even when the
     * healer is nearer, which is the entire point of the ability. */
    uint32_t forced = 0;
    if (npc->taunt_expires_at > 0.0) {
        if (t->now >= npc->taunt_expires_at) {
            npc->taunt_expires_at = 0.0;
            npc->taunt_source_id  = 0;
        } else {
            forced = npc->taunt_source_id;
        }
    }

    if (forced) {
        int d = tick_snapshot_find(s, forced);
        if (d >= 0 && !s->is_dead[d]) {
            t->has_target   = 1;
            t->target_dense = d;
            t->target_id    = forced;
            t->target_x     = s->pos_x[d];
            t->target_y     = s->pos_y[d];
            t->target_dist  = dist2d(npc->pos_x, npc->pos_y, t->target_x, t->target_y);
            return;
        }
        /* The taunter left or died; the taunt dies with them. */
        npc->taunt_expires_at = 0.0;
        npc->taunt_source_id  = 0;
    }

    float spawn_dist = dist2d(npc->pos_x, npc->pos_y, npc->spawn_x, npc->spawn_y);

    /* Hold the target already chosen while it stays in reach, so an NPC does not
     * re-pick every tick and jitter between two equidistant players. */
    if (npc->ai_target_id > 0) {
        int d = tick_snapshot_find(s, npc->ai_target_id);
        if (d >= 0 && !s->is_dead[d]) {
            float dd = dist2d(npc->pos_x, npc->pos_y, s->pos_x[d], s->pos_y[d]);
            int leashed = t->prof->leash_range > 0.0f && spawn_dist >= t->prof->leash_range;
            if (!leashed && dd < NPC_HOLD_AGGRO_RANGE) {
                t->has_target   = 1;
                t->target_dense = d;
                t->target_id    = npc->ai_target_id;
                t->target_x     = s->pos_x[d];
                t->target_y     = s->pos_y[d];
                t->target_dist  = dd;
                return;
            }
        }
    }

    int nearby[NPC_AGGRO_CANDIDATES];
    int n = tick_snapshot_query(s, npc->pos_x, npc->pos_y, t->prof->aggro_range,
                                nearby, NPC_AGGRO_CANDIDATES);

    float best_score = -1e30f;
    for (int k = 0; k < n; k++) {
        int d = nearby[k];
        if (s->is_dead[d]) continue;
        float dd = dist2d(npc->pos_x, npc->pos_y, s->pos_x[d], s->pos_y[d]);
        float score = priority_score(t, d, dd);
        if (score <= best_score) continue;
        best_score      = score;
        t->has_target   = 1;
        t->target_dense = d;
        t->target_id    = s->character_id[d];
        t->target_x     = s->pos_x[d];
        t->target_y     = s->pos_y[d];
        t->target_dist  = dd;
    }
}

/* --- Movement ------------------------------------------------------------- */

/** Step this NPC toward a point, stopping short of terrain. */
static void step_towards(NPCThink* t, float tx, float ty, float speed, float stop_at) {
    NPCEntity* npc = t->npc;
    float dx = tx - npc->pos_x, dy = ty - npc->pos_y;
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 0.001f) return;

    float move = speed * t->dt;
    if (move > len - stop_at) move = len - stop_at;
    if (move <= 0.0f) return;

    float nx = npc->pos_x + (dx / len) * move;
    float ny = npc->pos_y + (dy / len) * move;
    if (world_collision_check_box(nx, ny, npc->hitbox_radius) ||
        !world_coord_is_valid(nx, ny))
        return;

    npc->pos_x = nx;
    npc->pos_y = ny;
    npc->facing_x = dx / len;
    npc->facing_y = dy / len;
}

/** Move this NPC for one tick, relative to its target and its archetype. */
static void move_phase(NPCThink* t, float speed) {
    NPCEntity* npc = t->npc;

    /* Fleeing overrides the archetype: a Smoke Thief at 25% health is not
     * kiting, it is leaving. */
    if (npc->flee_until > t->now) {
        if (t->has_target) {
            float away_x = npc->pos_x * 2.0f - t->target_x;
            float away_y = npc->pos_y * 2.0f - t->target_y;
            step_towards(t, away_x, away_y, speed * 1.3f, 0.0f);
        }
        return;
    }

    if (!t->has_target) return;

    /* Erratic approach: the Failed Experiment does not walk a straight line, so
     * a player cannot lead it. Noise is a per-tick angular wobble rather than a
     * random walk, which keeps it approaching while making the path unreadable. */
    float tx = t->target_x, ty = t->target_y;
    if (t->prof->movement_noise > 0.0f) {
        float wobble = t->prof->movement_noise * frand_range(-1.0f, 1.0f) * world_tile_size();
        tx += wobble;
        ty += t->prof->movement_noise * frand_range(-1.0f, 1.0f) * world_tile_size();
    }

    switch (t->prof->movement) {
        case NPC_ARCH_FOLLOW: {
            /* Close far enough to use the *shortest* attack in the kit, not the
             * longest. A lunger carrying a one-tile swing that stopped at its
             * archetype's 1.5-tile preference would stand just outside its own
             * reach forever -- the exact silent failure the content validator's
             * R7 was written to catch, moved into the engine where it cannot
             * happen at all. The 0.9 leaves margin for a target that is walking.
             *
             * shortest_reach is zero only for a kit with nothing targeted, which
             * has no reason to approach; that falls through to the preference. */
            float stop = t->prof->preferred_range;
            float reach = t->prof->shortest_reach;
            if (reach > 0.0f) {
                float want = reach * 0.9f;
                if (stop <= 0.0f || want < stop) stop = want;
            } else if (stop <= 0.0f) {
                stop = world_tile_size();
            }
            if (t->target_dist > stop) step_towards(t, tx, ty, speed, stop);
            break;
        }

        case NPC_ARCH_MAINTAIN_RANGE: {
            float desired = t->prof->preferred_range;
            float retreat = t->prof->retreat_range > 0.0f
                          ? t->prof->retreat_range : NPC_RANGE_TOLERANCE;

            if (t->target_dist < desired - retreat) {
                float away_x = npc->pos_x * 2.0f - tx;
                float away_y = npc->pos_y * 2.0f - ty;
                step_towards(t, away_x, away_y, speed, 0.0f);
                /* Keep facing the threat while backing away from it. */
                float fx = tx - npc->pos_x, fy = ty - npc->pos_y;
                float fl = sqrtf(fx * fx + fy * fy);
                if (fl > 0.001f) { npc->facing_x = fx / fl; npc->facing_y = fy / fl; }
            } else if (t->target_dist > desired + NPC_RANGE_TOLERANCE) {
                step_towards(t, tx, ty, speed, desired);
            }
            break;
        }

        case NPC_ARCH_STATIONARY:
        default: {
            float fx = tx - npc->pos_x, fy = ty - npc->pos_y;
            float fl = sqrtf(fx * fx + fy * fy);
            if (fl > 0.001f) { npc->facing_x = fx / fl; npc->facing_y = fy / fl; }
            break;
        }
    }
}

/* --- Act phase ------------------------------------------------------------ */

/** Resolve a type-local ability slot through this NPC's swap table. */
static const NPCAbilityDef* ability_at(NPCThink* t, int slot) {
    int over = npc_world_ability_override(t->world, t->slot, slot);
    if (over >= 0) {
        const NPCAbilityDef* ab = npc_ai_registry_ability(over);
        if (ab) return ab;
    }
    if (slot < 0 || slot >= t->prof->ability_count) return NULL;
    return &t->prof->abilities[slot];
}

/** Shortlist of this NPC's ready abilities.
 *
 * Sized from npc_ai_widest_kit() rather than a compiled bound, and retained
 * because only the AI tick reaches this and that runs on the single gameplay
 * thread. It was a stack array sized by MAX_NPC_ABILITIES until that constant
 * stopped existing.
 */
static int* g_ready;
static int  g_ready_capacity;

int npc_behavior_ready(void) {
    int want = npc_ai_widest_kit();
    if (want <= g_ready_capacity) return g_ready_capacity > 0;

    int* grown = realloc(g_ready, (size_t)want * sizeof(*grown));
    if (!grown) {
        LOG_ERROR("[NPC_AI] could not size the ability shortlist to %d", want);
        return g_ready_capacity > 0;
    }
    g_ready = grown;
    g_ready_capacity = want;
    return 1;
}

/** Choose and deliver one ability, if any is ready, in range and permitted. */
static void act_phase(NPCThink* t) {
    int* ready = g_ready;
    int  ready_capacity = g_ready_capacity;
    NPCEntity* npc = t->npc;

    if (t->now < npc->recover_until) return;
    if (npc_is_action_locked(t->world, t->slot)) return;
    if (npc->flee_until > t->now) return;

    int ready_count = 0;
    for (int a = 0; a < t->prof->ability_count && a < t->cooldown_count &&
                    ready_count < ready_capacity; a++) {
        const NPCAbilityDef* ab = ability_at(t, a);
        if (!ab || !ab->src) continue;
        if (ab->src->delivery == NPC_DELIV_PASSIVE) continue;
        if (!npc_phase_allows(t, a)) continue;

        /* A self-directed ability -- a buff, a heal, a summon -- needs no target
         * and no reach. Everything else has to be able to touch someone. */
        int needs_target = !(ab->src->delivery == NPC_DELIV_BUFF ||
                             ab->src->delivery == NPC_DELIV_SUMMON ||
                             (ab->src->delivery == NPC_DELIV_ZONE && ab->range <= 0.0f));
        if (needs_target) {
            if (!t->has_target) continue;
            if (t->target_dist > ab->range) continue;
        }

        if ((t->now - t->cooldowns[a]) < npc_ability_cooldown(npc, ab)) continue;
        ready[ready_count++] = a;
    }

    if (ready_count == 0) return;

    int slot = ready[rand() % ready_count];
    const NPCAbilityDef* ab = ability_at(t, slot);
    float ax = t->has_target ? t->target_x : npc->pos_x;
    float ay = t->has_target ? t->target_y : npc->pos_y;
    npc_behavior_cast(t, ab, slot, ax, ay);
}

/* --- Casting resolution --------------------------------------------------- */

/** Complete an in-flight telegraph and queue its resolution. */
static void resolve_cast(NPCThink* t) {
    NPCEntity* npc = t->npc;
    const NPCAbilityDef* ab = ability_at(t, npc->ai_cast_ability_idx);
    if (!ab || !ab->src) {
        npc->ai_is_casting = 0;
        npc->ai_state = NPC_AI_AGGRO;
        return;
    }

    if (t->now - npc->ai_cast_start < npc->ai_cast_duration) return;

    const NPCAbilityDefn* s = ab->src;
    npc->ai_is_casting = 0;
    npc->ai_state = NPC_AI_AGGRO;
    if (s->recovery > 0.0f) npc->recover_until = t->now + s->recovery;

    int circular = (s->telegraph.shape == NPC_TELE_CIRCLE ||
                    s->telegraph.shape == NPC_TELE_CONE);
    float reach = npc->ai_cast_reach;

    /* The engine's stand-in for a travelling charge: the NPC arrives at the end
     * of the line when the cast resolves. A dash ability (V8) travels instead;
     * this is the shape Boar was authored against and it still ships. */
    if (s->telegraph.teleport_on_resolve) {
        float nx = npc->ai_cast_pos_x + npc->ai_cast_dir_x * reach;
        float ny = npc->ai_cast_pos_y + npc->ai_cast_dir_y * reach;
        if (!world_collision_check_box(nx, ny, npc->hitbox_radius) &&
            world_coord_is_valid(nx, ny)) {
            npc->pos_x = nx;
            npc->pos_y = ny;
        }
    }

    NPCDeferredAction d;
    npc_delivery_action_begin(t, &d, NPC_ACT_TELEGRAPH_RESOLVE);
    d.tresolve.ability_id   = ab->ability_id;
    d.tresolve.shape        = s->telegraph.shape;
    d.tresolve.pos_x        = npc->ai_cast_pos_x;
    d.tresolve.pos_y        = npc->ai_cast_pos_y;
    d.tresolve.dir_x        = npc->ai_cast_dir_x;
    d.tresolve.dir_y        = npc->ai_cast_dir_y;
    d.tresolve.radius       = circular ? reach : ab->telegraph_radius;
    d.tresolve.angle        = s->telegraph.angle;
    d.tresolve.width        = ab->telegraph_width;
    d.tresolve.length       = circular ? ab->telegraph_length : reach;
    d.tresolve.damage       = npc_ability_damage(npc, ab);
    d.tresolve.damage_type  = s->damage_type;
    d.tresolve.deals_damage = npc->ai_cast_resolves;
    d.tresolve.effect_first = s->effect_first;
    d.tresolve.effect_count = s->effect_count;
    npc_deferred_push(t->q, &d);

    if (npc->ai_cast_resolves)
        npc_delivery_components(t, ab, npc->ai_cast_pos_x, npc->ai_cast_pos_y);

    t->ability_used = 1;
}

/* --- Entry point ---------------------------------------------------------- */

/** Walk a leashed NPC home, healing it when it arrives. */
static void return_phase(NPCThink* t, float speed) {
    NPCEntity* npc = t->npc;
    float spawn_dist = dist2d(npc->pos_x, npc->pos_y, npc->spawn_x, npc->spawn_y);

    if (spawn_dist < NPC_RETURN_SNAP) {
        npc->pos_x = npc->spawn_x;
        npc->pos_y = npc->spawn_y;
        npc->ai_state = NPC_AI_IDLE;
        npc->ai_target_id = 0;
        npc->health = npc->max_health;
        return;
    }
    step_towards(t, npc->spawn_x, npc->spawn_y, speed * 1.5f, 0.0f);
}

void npc_behavior_think(NPCThink* t) {
    NPCEntity* npc = t->npc;
    t->ability_used = 0;

    float speed = t->prof->move_speed *
                  npc_speed_multiplier(t->world, t->slot, npc);
    int movement_locked = npc_is_movement_locked(t->world, t->slot,
                                                 t->prof->arch->movement_cc_immune);

    if (npc->ai_state == NPC_AI_CASTING && npc->ai_is_casting) {
        /* A casting NPC is locked in place: that commitment is what makes a
         * telegraph a decision the player gets to answer. */
        select_target(t);
        resolve_cast(t);
        return;
    }

    if (npc_behavior_advance_commitments(t)) {
        select_target(t);
        return;
    }

    if (npc->ai_state == NPC_AI_RETURNING) {
        if (!movement_locked) return_phase(t, speed);
        return;
    }

    select_target(t);

    float spawn_dist = dist2d(npc->pos_x, npc->pos_y, npc->spawn_x, npc->spawn_y);

    if (!t->has_target) {
        if (npc->ai_state == NPC_AI_AGGRO) {
            npc->ai_state = NPC_AI_RETURNING;
            npc->ai_target_id = 0;
        }
        return;
    }

    if (t->prof->leash_range > 0.0f && spawn_dist >= t->prof->leash_range) {
        npc->ai_state = NPC_AI_RETURNING;
        npc->ai_target_id = 0;
        t->has_target = 0;
        return;
    }

    npc->ai_state     = NPC_AI_AGGRO;
    npc->ai_target_id = t->target_id;

    if (!movement_locked) move_phase(t, speed);

    /* Movement changed the distance the act phase range-checks against. */
    t->target_dist = dist2d(npc->pos_x, npc->pos_y, t->target_x, t->target_y);

    act_phase(t);
}
