/** @file Expose world-server NPC management and basic attack resolution. */

#ifndef COMBAT_H
#define COMBAT_H

#include "combat_config.h"
#include "npc_snapshot.h"
#include "npc_world.h"
#include "types.h"
#include "player_data.h"

#include <stdio.h>
#include <stdint.h>

// return zero and retain compiled defaults when loading fails
int combat_profiles_load(const char* path);

/** Return the basic-attack profile for a race.
 *
 * @return A profile that is always safe to read; races without an entry in
 *         attack_profiles.json get a plain melee fallback.
 */
const RaceAttackProfile* combat_profile_for_race(uint32_t race_id);

/* NPC pool lifetime, spawning, removal, and locking live in npc_world.h. */

/** Validate and snapshot an attack intent, or send its rejection.
 *
 * Runs on a network loop thread, not the gameplay thread, so it scans the pool
 * under its read lock rather than using the gameplay tick snapshot — that
 * snapshot is owned by one thread and its grid keeps query scratch inside itself.
 */
void combat_handle_attack_intent(NPCWorld* world,
                                 int client_fd,
                                 uint32_t attacker_id,
                                 AttackIntentPacket* pkt);

void combat_handle_cast_cancel(int client_fd, uint32_t attacker_id);

/** Resolve elapsed casts from the combat update thread.
 *
 * @param world  NPC pool that owns the entities damage is applied to.
 * @param npcs  This tick's NPC snapshot, used to narrow targets before locking.
 */
void combat_tick(NPCWorld* world, NPCTickSnapshot* npcs);

#endif // COMBAT_H