#ifndef NPC_DEFERRED_H
#define NPC_DEFERRED_H

/** @file Queue the work an NPC's think phase cannot do while holding its own lock.
 *
 * The pool's stated lock order is pool-read then one slot, never two slots. Every
 * interesting thing an NPC ability does touches something *other* than the NPC
 * casting it -- damage a player, buff an ally, lay a zone, spawn a summon -- so
 * doing any of it inline is a lock-order inversion or a deadlock, not a style
 * preference.
 *
 * The think phase therefore describes what it wants and appends it here; the tick
 * releases every lock and then performs the queue in order. This is the mechanism
 * that already carried telegraph damage against players, extended to carry the
 * rest.
 *
 * The queue is sized from the pool rather than from a constant: an NPC can
 * contribute at most npc_registry_max_actions_per_npc() actions in a tick, so
 * `capacity * that` cannot overflow. It matters that it cannot -- the previous
 * fixed 128 entries sat below even the default pool of 256, and past the limit a
 * telegraph's start was sent and its resolve dropped, leaving a warning painted
 * on the ground forever and its damage never dealt.
 */

#include "npc_snapshot.h"
#include "npc_world.h"
#include "tick_snapshot.h"

#include <stdint.h>

/** Name one kind of after-the-locks work. */
typedef enum {
    NPC_ACT_TELEGRAPH_START = 0,  /**< Broadcast a telegraph's warning. */
    NPC_ACT_TELEGRAPH_RESOLVE,    /**< Broadcast its resolution and damage what it covers. */
    NPC_ACT_PROJECTILE,      /**< Spawn one projectile. */
    NPC_ACT_CONTACT,               /**< V1: damage players touching this NPC. */
    NPC_ACT_BUFF,              /**< V5/V12: apply effects to this NPC or its allies. */
    NPC_ACT_HEAL,              /**< V12: heal this NPC or its allies. */
    NPC_ACT_ZONE,                  /**< V6: place ground zones owned by this NPC. */
    NPC_ACT_SUMMON,                /**< V7: spawn summons parented to this NPC. */
    NPC_ACT_COUNT
} NPCDeferredType;

/** Describe one queued action.
 *
 * The acting NPC's identity and position are hoisted out of the union because
 * every action needs them: the broadcast radius is measured from the NPC, and
 * the flush re-acquires the NPC by identifier when it has to write back.
 */
typedef struct {
    uint8_t  type;              /**< NPCDeferredType. */
    uint32_t npc_id;
    int      npc_slot;
    float    npc_x, npc_y;
    uint16_t npc_type_id;
    int      faction_index;     /**< For faction-scoped ally effects; -1 when unknown. */

    union {
        /** Telegraph warning, broadcast to nearby players. */
        struct {
            uint16_t ability_id;
            uint8_t  shape;
            float    pos_x, pos_y;
            float    dir_x, dir_y;
            float    radius, angle, width, length;
            float    cast_time;
        } tstart;

        /** Telegraph resolution: broadcast, then damage what the shape covers. */
        struct {
            uint16_t ability_id;
            uint8_t  shape;
            float    pos_x, pos_y;
            float    dir_x, dir_y;
            float    radius, angle, width, length;
            int      damage;
            uint8_t  damage_type;
            uint8_t  deals_damage;   /**< 0 for a feint, which warns and does nothing. */
            int      effect_first, effect_count;
        } tresolve;

        /** One projectile. A burst queues one of these per shot, across ticks. */
        struct {
            uint16_t ability_id;
            float    origin_x, origin_y;
            float    target_x, target_y;
            int      damage;
            uint8_t  damage_type;
            float    speed, width, range;
            int      effect_first, effect_count;
        } proj;

        /** V1 contact: damage every player whose body overlaps this NPC's. */
        struct {
            uint16_t ability_id;
            int      damage;
            uint8_t  damage_type;
            float    reach;          /**< NPC hitbox plus the ability's touch range. */
            int      effect_first, effect_count;
        } contact;

        /** V5 buff or aura, applied to this NPC or to allies around it. */
        struct {
            uint16_t ability_id;
            float    radius;         /**< 0 with target_self means self only. */
            float    duration;
            uint8_t  target_self;
            uint8_t  faction_only;
            uint8_t  single_ally;    /**< Tether picks one, not the whole pack. */
            uint8_t  ready_allies;   /**< V12: clear their cooldowns so they act now. */
            uint8_t  retarget_allies;/**< V12: point them at this NPC's target. */
            uint32_t target_id;      /**< Whom to retarget them onto; 0 for none. */
            int      effect_first, effect_count;
        } buff;

        /** V12 heal, aimed at this NPC, an area, or the worst-off ally. */
        struct {
            uint16_t ability_id;
            float    radius;
            int      amount;
            int      percent;        /**< Tenths of a percent of max health. */
            uint8_t  lowest_ally_only;
            uint8_t  target_self;
        } heal;

        /** V6 ground zone. `count` zones are scattered within `spread`. */
        struct {
            uint16_t ability_id;
            float    pos_x, pos_y;
            float    radius;
            float    duration;
            float    tick_rate;
            int      count;
            float    spread;
            int      effect_first, effect_count;
        } zone;

        /** V7 summon. Type indices are registry indices, already resolved. */
        struct {
            uint16_t ability_id;
            int      type_index[4];
            int      type_count;
            int      count_min, count_max;
            float    spread;
        } summon;
    };
} NPCDeferredAction;

/** Hold one tick's worth of queued actions. Storage is retained across ticks. */
typedef struct {
    NPCDeferredAction* items;
    int count;
    int capacity;
} NPCDeferredQueue;

/** Append one action, or log and drop it when the queue is full.
 *
 * Full is only reachable when the sizing allocation failed, and it is never
 * silent: a dropped resolve is a telegraph that warns and never lands.
 */
void npc_deferred_push(NPCDeferredQueue* q, const NPCDeferredAction* item);

/** Perform every queued action and empty the queue.
 *
 * The caller must hold neither the NPC-world lock nor any player lock: this is
 * the phase that exists precisely so that the work can take them.
 *
 * @param world    NPC pool the acting NPCs belong to.
 * @param players  This tick's player snapshot; NULL performs nothing.
 * @param npcs     This tick's NPC snapshot, for ally-directed actions; may be NULL.
 */
void npc_deferred_flush(NPCDeferredQueue* q, NPCWorld* world,
                        TickSnapshot* players, NPCTickSnapshot* npcs);

#endif // NPC_DEFERRED_H
