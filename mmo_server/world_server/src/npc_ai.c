/**
 * @file
 * Orchestrate the NPC gameplay tick: think, trigger, phase, flush, reap.
 *
 * What is left here after the verbs landed. The per-NPC state machine moved to
 * npc_behavior.c, the deliveries to npc_delivery.c, the trigger table to
 * npc_triggers.c and the queue's flush to npc_ai_flush.c. This file owns the
 * order those run in, and the order is the interesting part:
 *
 *   1. effects tick    -- expiry and damage-over-time, under the slot lock
 *   2. think and act   -- target, move, choose, describe into the queue
 *   3. triggers        -- evaluated against the target the act phase just chose
 *   4. phase advance   -- after the act phase, so `after_uses` sees this tick
 *   5. release locks
 *   6. flush           -- everything that touches anything but the acting NPC
 *   7. reap orphans    -- summons whose summoner is gone, under the write lock
 *
 * Steps 6 and 7 are outside the scan for the same reason: both take locks the
 * scan already holds or holds the wrong one of. That is not an implementation
 * detail, it is the pool's stated lock order, and it is why the queue exists.
 */

#include "npc_ai.h"
#include "npc_behavior.h"
#include "npc_deferred.h"
#include "npc_effects.h"
#include "npc_summon.h"
#include "npc_triggers.h"
#include "npc_registry.h"
#include "log.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

static double get_time(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* --- The deferred queue's storage ----------------------------------------
 *
 * Sized from the pool times the most actions one NPC can contribute in a tick,
 * both read from the registry rather than fixed. The previous version was 128
 * entries, below even the default pool of 256, and the overflow was silent: past
 * the limit a telegraph's start was sent and its resolve dropped, leaving a
 * warning painted on the ground forever and its damage never dealt.
 *
 * Retained between ticks, so the allocation happens once, on the first tick
 * after the pool's capacity is known.
 */
static NPCDeferredAction* g_dq_items;
static int             g_dq_capacity;

/** Reset a queue and ensure it can hold this tick's worst case. */
static void dq_init(NPCDeferredQueue* q, int capacity) {
    if (capacity > g_dq_capacity) {
        NPCDeferredAction* grown = realloc(g_dq_items, (size_t)capacity * sizeof(*grown));
        if (grown) {
            g_dq_items    = grown;
            g_dq_capacity = capacity;
        } else {
            LOG_ERROR("[NPC_AI] could not size the deferred queue to %d actions; "
                      "holding at %d", capacity, g_dq_capacity);
        }
    }
    q->items    = g_dq_items;
    q->capacity = g_dq_capacity;
    q->count    = 0;
}

/* --- The summon census ---------------------------------------------------
 *
 * A summon must go when its summoner does, and the obvious implementation --
 * each summon reading its parent's state -- is two slot locks at once, which the
 * pool forbids. So the scan records one entry per summon and the reap runs
 * afterwards, with no lock held, taking the write lock removal needs.
 *
 * The same pass recounts each summoner's budget from what is actually alive,
 * rather than decrementing on death. A counter maintained by matched increments
 * and decrements drifts the first time a path is missed, and the symptom -- a
 * boss that stops summoning halfway through a fight -- is indistinguishable from
 * intended behaviour.
 */
static uint32_t* g_summon_child;
static uint32_t* g_summon_parent;
static uint8_t*  g_summon_alive;
static int       g_summon_capacity;

/** Grow the summon-census buffers to the pool's capacity. Returns the capacity. */
static int summon_reserve(int capacity) {
    if (capacity <= g_summon_capacity) return g_summon_capacity;

    uint32_t* c = realloc(g_summon_child,  (size_t)capacity * sizeof(uint32_t));
    uint32_t* p = realloc(g_summon_parent, (size_t)capacity * sizeof(uint32_t));
    uint8_t*  a = realloc(g_summon_alive,  (size_t)capacity * sizeof(uint8_t));
    if (c) g_summon_child  = c;
    if (p) g_summon_parent = p;
    if (a) g_summon_alive  = a;
    if (!c || !p || !a) {
        LOG_ERROR("[NPC_AI] could not size the summon-census buffers to %d", capacity);
        return g_summon_capacity;
    }
    g_summon_capacity = capacity;
    return g_summon_capacity;
}

/* --- The tick -------------------------------------------------------------- */

void npc_ai_tick(NPCWorld* world, TickSnapshot* snap, NPCTickSnapshot* npcs,
                 double delta_time) {
    double now = get_time();
    float  dt  = (float)delta_time;

    if (!snap || snap->count == 0) return;

    int capacity = npc_world_capacity(world);

    NPCDeferredQueue q;
    dq_init(&q, capacity * npc_registry_max_actions_per_npc());
    if (!q.items) return;                 /* nothing can be deferred, so nothing may start */
    if (!npc_behavior_ready()) return;    /* no shortlist, so no ability may be chosen */

    int summon_cap = summon_reserve(capacity);
    int summon_count = 0;

    /* Every NPC has to think, so this phase iterates rather than queries -- there
     * is no position to query from. Two things make that affordable.
     *
     * The locking: the pool read lock plus one NPC's mutex at a time, instead of
     * one exclusive lock across the whole pass. An AI tick no longer blocks
     * combat, projectiles, or any inbound packet for its full duration.
     *
     * And what it walks. This used to run over all `capacity` slots, twenty times
     * a second, whatever fraction of them held an NPC -- a pool sized for a
     * launch-day crowd cost the same to think for whether two hundred NPCs were
     * spawned or none. It walks the occupied-slot list now, so the cost tracks
     * the NPCs that exist. */
    npc_world_read_begin(world);

    int live_count = 0;
    const int* live = npc_world_live_slots(world, &live_count);

    for (int i = 0; i < live_count; i++) {
        int n = live[i];
        NPCEntity* npc = npc_world_slot(world, n);
        if (!npc || npc->id == 0) continue;   /* unlocked pre-filter only */

        /* The base pointer is fixed for the pool's life, so taking it before the
         * slot lock is safe; its *contents* are guarded by that lock, exactly
         * like the NPCEntity's own fields. */
        double* cds      = npc_world_cooldowns(world, n);
        int     cd_count = npc_world_max_abilities(world);

        npc_world_slot_lock(world, n);

        if (npc->id == 0 || npc->npc_type_id == 0) goto next_npc;

        const NPCAIProfile* prof = npc_ai_get_profile(npc->npc_type_id);
        if (!prof) goto next_npc;

        NPCThink t = {0};
        t.world          = world;
        t.slot           = n;
        t.npc            = npc;
        t.prof           = prof;
        t.players        = snap;
        t.npcs           = npcs;
        t.now            = now;
        t.dt             = dt;
        t.q              = &q;
        t.cooldowns      = cds;
        t.cooldown_count = cd_count;
        t.target_dense   = -1;
        t.faction_index  = npc_faction_of_type(npc->npc_type_id);

        if (npc->parent_npc_id != 0 && summon_count < summon_cap) {
            g_summon_child[summon_count]  = npc->id;
            g_summon_parent[summon_count] = npc->parent_npc_id;
            g_summon_alive[summon_count]  = npc->is_alive;
            summon_count++;
        }

        if (!npc->is_alive) {
            /* A corpse still evaluates its death triggers, once. This is what
             * makes an on-death summon work without a special case at each of
             * the four places NPC health is written. */
            npc_triggers_on_death(&t);
            goto next_npc;
        }

        /* Stagger initial ability cooldown phases per NPC, so a pack spawned
         * together does not fire in lockstep forever. */
        if (!npc->ai_cd_seeded) {
            for (int a = 0; a < prof->ability_count && a < cd_count; a++) {
                double cd = prof->abilities[a].src->cooldown;
                if (cd > 0.0)
                    cds[a] = now - cd * ((double)(rand() % 1000) / 1000.0);
            }
            npc->ai_cd_seeded = 1;
        }

        /* Expiry and damage-over-time first: an NPC that a bleed kills this tick
         * should not also get to act. */
        int dot = 0;
        if (npc_effects_tick(world, n, npc, dt, &dot)) {
            npc->is_alive   = 0;
            npc->death_time = now;
            npc_triggers_on_death(&t);
            goto next_npc;
        }

        npc_behavior_think(&t);
        npc_triggers_evaluate(&t);
        npc_phase_advance(&t);

next_npc:
        /* Single exit for the loop body. The body is long and branches often;
         * with a plain `continue` at each of those branches, every future edit
         * would have to remember to drop this NPC's lock on the way out. */
        npc_world_slot_unlock(world, n);
    }

    npc_world_read_end(world);

    npc_deferred_flush(&q, world, snap, npcs);

    if (summon_count > 0)
        npc_summon_reap(world, g_summon_child, g_summon_parent,
                        g_summon_alive, summon_count);
}
