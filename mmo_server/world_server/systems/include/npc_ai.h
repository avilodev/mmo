/** @file Own the runtime NPC behaviour table and the AI tick that drives it. */

#ifndef NPC_AI_H
#define NPC_AI_H

#include "npc_world.h"
#include "npc_registry.h"
#include "npc_snapshot.h"
#include "tick_snapshot.h"
#include <stdint.h>

/* Nothing here is bounded at compile time.
 *
 * There used to be a MAX_NPC_AI_PROFILES of 32 and a MAX_NPC_ABILITIES of 4, and
 * content past either was dropped with a log line -- which reads, in game, as a
 * mob that will not aggro or a mini-boss missing half its kit. The profile table
 * is built from the loaded registry now and sized to it, so the only limit on how
 * many enemies a world can have is how many are written down.
 */

/** Identify the current state of an NPC's behavior machine. */
typedef enum {
    NPC_AI_IDLE       = 0,   // Standing at spawn, no target
    NPC_AI_AGGRO      = 1,   // Has a target, chasing/attacking
    NPC_AI_RETURNING  = 2,   // Lost target or leashed, walking back to spawn
    NPC_AI_CASTING    = 3    // Casting a telegraph ability (locked in place)
} NPCAIState;

/** Define one of a type's abilities in the units the tick works in.
 *
 * Deliberately thin. Everything an ability *is* -- its delivery, its damage, its
 * telegraph shape, its effect span, its summon list -- stays on the registry row
 * this points at, so there is exactly one copy of it and a retune is one edit.
 * What is duplicated here is only what has to change form: `enemy_types.txt`
 * states reach in tiles and the tick wants world units, and npc_ai_profile.c is
 * the single place that multiplies, against world_tile_size().
 *
 * A field named without `_tiles` on the registry row (a duration, an angle, a
 * projectile speed, a shot count) needs no conversion and is read through `src`.
 */
typedef struct {
    /** Slot index plus one. Zero stays "no ability" on the wire, as the client
     * expects, and the client uses this for VFX lookup. */
    uint16_t ability_id;

    /** The registry row. Never NULL for a slot below ability_count. */
    const NPCAbilityDefn* src;

    /** World-unit conversions of every *_tiles field `src` carries. */
    float range;
    float telegraph_radius;
    float telegraph_width;
    float telegraph_length;
    float zone_impact_radius;
    float zone_self_radius;
    float buff_radius;
    float heal_radius;
    float displace_distance;
    float summon_spread;
} NPCAbilityDef;

/** Associate movement, aggro, and ability settings with one NPC type. */
typedef struct {
    uint16_t npc_type_id;

    /** The registry rows this profile was composed from. Held rather than copied
     * so triggers, phases, mitigation and affixes read one source of truth. */
    const NPCTypeDef*      type;
    const NPCArchetypeDef* arch;

    uint8_t movement;               /**< NPCArchMovement. */
    uint8_t target_priority;        /**< NPCTargetPriority. */

    /** Distances in world units; the archetype states them in tiles. */
    float move_speed;
    float preferred_range;
    float aggro_range;
    float leash_range;              /**< 0 means this archetype never leashes. */
    float retreat_range;
    float ally_affinity_range;
    float hop_distance;             /**< 0 disables the skirmisher hop. */
    float hop_threat_range;
    float hop_cooldown;
    float movement_noise;

    /** The closest an ability in this kit wants its target, in world units.
     *
     * A chaser closes to this rather than to its archetype's preference, so an
     * enemy carrying a one-tile swing walks into range of it instead of holding
     * at the distance its longest attack would prefer. Zero when the kit is
     * entirely self-directed. */
    float shortest_reach;

    /** Abilities in world units. Points into one flat allocation shared by every
     * profile; the table owns it and frees it wholesale. */
    const NPCAbilityDef* abilities;
    int                  ability_count;
} NPCAIProfile;

/** Build the runtime behaviour table from the loaded content registry.
 *
 * Call after npc_registry_load() and npc_content_validate(): this reads the
 * composed registry rather than parsing a file, which is why the hand-rolled JSON
 * scanner that used to live in npc_ai.c is gone.
 *
 * Distances arrive in tiles and are converted here, once, against
 * world_tile_size() -- so a world with a tile size other than 16 does not need
 * every range in every data file rewritten.
 *
 * @return 1 when every type converted, or 0 when a type names an archetype that
 *         did not resolve, which is refused rather than silently dropped.
 */
int npc_ai_init(void);

void npc_ai_cleanup(void);

/** Bound each NPC's nearest-first candidate search without changing reachability.
 *
 * A truncating cap, and a safe one, for a reason worth stating rather than
 * assuming: the query is nearest-first and the caller takes the first *living*
 * candidate, so the only way 16 is too few is 16 corpses stacked closer to the
 * NPC than any living player. Dead players do not stack -- they respawn on a
 * timer and away from where they fell -- so this bound is physical rather than
 * chosen, and truncation drops only candidates the search had already passed
 * over.
 *
 * Raise it if the death model ever leaves bodies in place. An undocumented safe
 * cap reads exactly like an unsafe one, which is how the unsafe ones got in.
 */
#define NPC_AGGRO_CANDIDATES 16

/** Advance NPC targeting, movement, cooldowns, triggers, and casts at 20 Hz.
 *
 * @param world  NPC pool whose entities are updated.
 * @param snap   Tick-wide player snapshot; NULL or empty snapshots skip the update.
 * @param npcs   Tick-wide NPC snapshot, for ally queries and summon budgets; may be NULL.
 * @param delta_time  Elapsed tick time in seconds.
 */
void npc_ai_tick(NPCWorld* world, TickSnapshot* snap, NPCTickSnapshot* npcs,
                 double delta_time);

/** Return one registry ability, converted to world units.
 *
 * The abilities a type declares are reached through its profile. This is for the
 * two cases that reach past a type's own list: a `cast` trigger naming any
 * ability in the registry, and a `swap_ability` trigger whose target the type
 * never declared.
 *
 * @param registry_index  Index into the registry's ability table.
 * @return                The converted ability, or NULL when out of range.
 */
const NPCAbilityDef* npc_ai_registry_ability(int registry_index);

/** Return a registry-owned profile or NULL when absent. */
const NPCAIProfile* npc_ai_get_profile(uint16_t npc_type_id);

/** Report the widest ability_count across loaded profiles.
 *
 * The tick sizes its per-NPC shortlist from this. Zero before npc_ai_init().
 */
int npc_ai_widest_kit(void);

#endif // NPC_AI_H
