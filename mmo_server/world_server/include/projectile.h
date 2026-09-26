/** @file Define player and NPC projectiles, collision updates, and broadcasts. */

#ifndef PROJECTILE_H
#define PROJECTILE_H

#include "broadcast_snapshot.h"
#include "combat_config.h"
#include "npc_snapshot.h"
#include "npc_world.h"
#include "tick_snapshot.h"
#include "ability_def.h"
#include "damage_model.h"
#include "types.h"

#include <stdint.h>
#include <pthread.h>

/** Bound active projectiles and their visibility range in world units. */
#define MAX_PROJECTILES           128
#define PROJECTILE_VIEW_RANGE    2000.0f

/** Identify whether a player or NPC owns a projectile. */
typedef enum {
    PROJECTILE_OWNER_PLAYER = 0,
    PROJECTILE_OWNER_NPC    = 1
} ProjectileOwnerType;

/** Identify why a projectile left the world. */
typedef enum {
    PROJECTILE_DESTROY_EXPIRED  = 0,
    PROJECTILE_DESTROY_HIT      = 1,
    PROJECTILE_DESTROY_CANCELLED = 2
} ProjectileDestroyReason;

/** Track one active projectile's ownership, physics, damage, and effects. */
typedef struct {
    uint8_t     is_active;
    uint32_t    projectile_id;

    /** Retain ownership and client feedback routing. */
    uint8_t     owner_type;         // ProjectileOwnerType
    uint32_t    owner_id;           // Character ID or NPC ID
    uint16_t    ability_id;         // Source ability (client uses for VFX)
    int         owner_fd;           // Socket fd of the owner (for hit feedback)

    /** Track movement and traveled range in world units. */
    float       pos_x, pos_y;
    float       dir_x, dir_y;      // Unit direction vector
    float       speed;              // World units per second
    float       width;              // Hitbox width
    float       max_range;
    float       distance_traveled;

    int         damage;
    AbilityDamageType damage_type;
    AbilityBonusDamageDef bonus_damage;

    /** Snapshot the caster's attributes, indexed by StatId, as they were at launch.
     *
     * Damage resolves at impact, which may be well after the cast, so the snapshot is
     * what makes a projectile's damage independent of anything that happens to the
     * caster while it is in flight. */
    int         caster_stats[STAT_COUNT];
    int         damage_stat;        // StatType int — which stat scales this projectile's damage
    /** Snapshot the caster's outgoing damage modifiers at launch, for the same reason. */
    DamageModifiers caster_mods;

    /** Retain status effects applied on impact. */
    AbilityEffectDef effects[MAX_ABILITY_EFFECTS];
    uint8_t     effect_count;
} Projectile;

/** Supply immutable inputs used to create one projectile. */
typedef struct {
    uint8_t     owner_type;         // PROJECTILE_OWNER_PLAYER or _NPC
    uint32_t    owner_id;
    uint16_t    ability_id;
    int         owner_fd;

    float       origin_x, origin_y;
    float       aim_x, aim_y;

    float       speed;
    float       width;
    float       max_range;

    int         damage;
    AbilityDamageType damage_type;
    AbilityBonusDamageDef bonus_damage;

    int         caster_stats[STAT_COUNT];
    int         damage_stat;        // StatType int — which stat scales this projectile's damage
    DamageModifiers caster_mods;

    AbilityEffectDef effects[MAX_ABILITY_EFFECTS];
    uint8_t     effect_count;
} ProjectileSpawnInfo;

void projectile_init(void);

void projectile_cleanup(void);

// return the assigned projectile identifier or zero on failure
uint32_t projectile_spawn(const ProjectileSpawnInfo* info);

/** Bound nearest-first player collision candidates without limiting reach.
 *
 * A truncating cap, and a safe one, because the bound is the hitbox rather than
 * the number: the query radius is one projectile's collision extent, and 16
 * entities simultaneously inside a projectile's hitbox is not a reachable world
 * state at any population -- bodies occupy space. Nearest-first besides, so
 * anything dropped was further from the projectile than 16 things it already
 * hit.
 *
 * Revisit alongside any change that grows projectile width substantially.
 */
#define PROJECTILE_HIT_CANDIDATES 16

// update movement, collision, and damage from the 20 Hz combat tick
/** Advance projectiles, resolve collisions, and queue their broadcasts.
 *
 * @param world  NPC pool that owns the entities damage is applied to.
 * @param snap  This tick's player snapshot, for NPC-owned projectiles.
 * @param npcs  This tick's NPC snapshot, for player-owned projectiles.
 * @param delta_time  Elapsed tick time in seconds.
 */
void projectile_tick(NPCWorld* world, TickSnapshot* snap, NPCTickSnapshot* npcs,
                     double delta_time);

// broadcast against the pass snapshot after releasing projectile locks
/** Send projectile updates to one shard of the broadcast snapshot.
 *
 * @param snapshot  Immutable pass snapshot of the players to send to.
 * @param shard  Shard index; handles players whose fd matches this shard.
 * @param shard_count  Total shards; pass 1 to send to everyone from one thread.
 */
void projectile_broadcast(const BroadcastSnapshot* snapshot, int shard, int shard_count);

void projectile_remove(uint32_t projectile_id);

#endif // PROJECTILE_H
