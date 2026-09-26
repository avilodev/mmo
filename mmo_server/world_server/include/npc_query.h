#ifndef NPC_QUERY_H
#define NPC_QUERY_H

/** @file A shared, read-only spatial index of living NPCs.
 *
 * npc_snapshot.h builds the same kind of index, but it belongs to the single
 * gameplay thread: its grid keeps query scratch inside itself, so it cannot be
 * read from anywhere else. That left the packet path with nothing, and
 * combat_handle_attack_intent() answered every attack packet by scanning the
 * entire NPC pool and taking each NPC's mutex in turn -- on a network loop
 * thread, once per attack, at whatever rate players choose to attack.
 *
 * This is the index the packet path can use. The gameplay thread publishes it
 * once per tick; loop threads query it under a read lock, with their own
 * scratch, and never touch the pool to decide who is a candidate.
 *
 * The published values are a copy taken at the top of a tick. That makes them
 * the right thing to reject candidates with -- a distance test, a cone test --
 * and the wrong thing to apply damage to. Resolve a survivor through
 * npc_world_acquire_slot() with the slot and id recorded here; that is what
 * re-checks the NPC is still there and still the same one.
 */

#include "npc_world.h"

#include <stdint.h>

/** One candidate returned by a query, copied out under the read lock. */
typedef struct {
    int      slot;           /**< Pool slot, for npc_world_acquire_slot(). */
    uint32_t id;
    uint16_t npc_type_id;    /**< Resolves faction and role without a pool read. */
    float    pos_x, pos_y;
    float    hitbox_radius;
} NpcQueryHit;

/** Allocate the index for a pool.
 *
 * @return 1 on success, or 0 on allocation failure.
 */
int npc_query_init(const NPCWorld* world);

/** Release the index. */
void npc_query_shutdown(void);

/** Resample living NPCs and publish a new index.
 *
 * Called once per gameplay tick, from the gameplay thread only. The rebuild
 * happens into a back buffer with no lock held; only the swap takes the write
 * lock, so a publish never stalls a packet thread for longer than a pointer
 * exchange.
 */
void npc_query_publish(NPCWorld* world);

/** Collect living NPCs within a radius, nearest first.
 *
 * Safe to call from any thread. The results are a copy; nothing in them points
 * into the index.
 *
 * @param out      Buffer receiving hits.
 * @param max_out  Its capacity.
 * @return         Number of hits written.
 */
int npc_query_near(float x, float y, float radius, NpcQueryHit* out, int max_out);

/** Look one NPC up by identifier in the published index.
 *
 * Safe to call from any thread. Answers "where is the thing I am running from"
 * and "whose side is the thing that charmed me on" -- both asked on a network
 * thread, where scanning the pool is exactly what this index exists to avoid.
 *
 * @param out  Receives the hit when the NPC is found.
 * @return     1 when the index holds that identifier, else 0.
 */
int npc_query_lookup(uint32_t npc_id, NpcQueryHit* out);

/** The largest hitbox radius in the published index.
 *
 * Query radii must be widened by this: a shape test is against an NPC's edge,
 * but the index holds its centre, so a search of exactly `range` would miss a
 * large NPC whose body is in range while its centre is not.
 */
float npc_query_max_hitbox_radius(void);

/** Living NPCs in the published index. For tests and metrics. */
int npc_query_count(void);

#endif // NPC_QUERY_H
