#ifndef ZONE_OWNER_H
#define ZONE_OWNER_H

/** @file Own every ground zone in the world, whoever cast it (V6).
 *
 * Zones used to live inside ability_handler.c, which meant they were a player
 * mechanic that NPCs had no way to reach. They are one pool here, with one
 * capacity and one per-caster cap, because there is exactly one reason to keep
 * them apart -- who cast it -- and that is a field, not a second pool.
 *
 * The per-caster cap is the part that only matters once NPCs cast them. A Rot
 * Crawler lays a zone every tick while it moves, and a Firebrand lays five per
 * cast; without a cap, two of either would own the whole pool and every other
 * zone in the world -- including the player's -- would silently fail to spawn.
 * The cap retires that caster's oldest zone instead, so a trail behaves like a
 * trail rather than like a pool exhaustion bug.
 *
 * Effects are directed by owner. A player's zone applies its effects to NPCs and
 * only its `reapply` effects to players, which is what makes a healing zone heal
 * the party standing in it; an NPC's zone applies to players and never to its own
 * allies, because a Bio-Caster's puddle poisoning its own pack is not a mechanic
 * anyone asked for.
 */

#include "ability_def.h"
#include "ability_handler.h"
#include "npc_snapshot.h"
#include "npc_world.h"
#include "tick_snapshot.h"

#include <stdint.h>

/** Identify what kind of entity owns a zone. */
typedef enum {
    ZONE_OWNER_PLAYER = 0,
    ZONE_OWNER_NPC    = 1
} ZoneOwnerType;

/** Cap zones per caster when a world configuration does not say.
 *
 * Eight covers a Firebrand's five-zone cast plus a moment of overlap with the
 * previous one, and stops a per-tick trail from monopolising the pool.
 */
#define ZONE_PER_OWNER_DEFAULT 8

/** Refuse an absurd configured per-owner cap. */
#define ZONE_PER_OWNER_MAX 256

/** Supply everything one zone needs, from either kind of caster. */
typedef struct {
    uint8_t  owner_type;        /**< ZoneOwnerType. */
    uint32_t caster_id;
    uint16_t ability_id;

    float pos_x, pos_y;
    float radius;
    float duration;
    float tick_rate;            /**< 0 takes the first effect's tick rate, or 1s. */

    uint8_t has_collision;
    int     hp;                 /**< 0 for an indestructible zone. */
    int     healing_per_tick;

    const AbilityEffectDef* effects;
    int                     effect_count;
} ZoneSpawnInfo;

/** Allocate the zone pool.
 *
 * @param capacity   Requested slots; 0 takes ZONE_CAPACITY_DEFAULT, clamped to
 *                   ZONE_CAPACITY_MAX.
 * @param per_owner  Zones one caster may hold; 0 takes ZONE_PER_OWNER_DEFAULT.
 * @return           1 on success, or 0 when allocation fails.
 */
int zone_pool_init(int capacity, int per_owner);

/** Report the pool's allocated capacity. */
int zone_pool_capacity(void);

/** Report the configured per-caster cap. */
int zone_pool_per_owner(void);

void zone_pool_shutdown(void);

/** Create one zone and announce it to everyone who can see where it landed.
 *
 * When the caster is already at its per-caster cap, its oldest zone is retired
 * to make room, so a caster that lays zones faster than they expire keeps a
 * moving trail rather than failing.
 *
 * @return The zone identifier, or 0 when the pool is full.
 */
uint32_t zone_create(const ZoneSpawnInfo* info);

/** Advance every zone's lifetime and apply its periodic effects.
 *
 * @param world    NPC pool that owns the entities effects are applied to.
 * @param players  This pass's player snapshot; may be NULL.
 * @param npcs     This pass's NPC snapshot; may be NULL.
 */
void zone_tick(NPCWorld* world, TickSnapshot* players, NPCTickSnapshot* npcs,
               double delta_time);

#endif // ZONE_OWNER_H
