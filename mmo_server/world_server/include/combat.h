/** @file Expose world-server NPC management and basic attack resolution. */

#ifndef COMBAT_H
#define COMBAT_H

#include "combat_config.h"
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

void combat_npc_init(NPCWorld* world);

// return the assigned NPC identifier or zero on failure
uint32_t combat_npc_spawn(NPCWorld* world,
                          const char* name,
                          float x, float y,
                          int health,
                          float hitbox_radius,
                          uint32_t dialogue_id,
                          uint8_t is_interactable,
                          uint16_t npc_type_id,
                          float respawn_time,
                          uint8_t category);

// return a world-owned NPC while the caller holds world->lock
NPCEntity* combat_npc_find(NPCWorld* world, uint32_t npc_id);

void combat_npc_remove(NPCWorld* world, uint32_t npc_id);

// validate and snapshot an attack intent or send its rejection
void combat_handle_attack_intent(NPCWorld* world,
                                 int client_fd,
                                 uint32_t attacker_id,
                                 AttackIntentPacket* pkt);

void combat_handle_cast_cancel(int client_fd, uint32_t attacker_id);

// resolve elapsed casts from the combat update thread
void combat_tick(NPCWorld* world);

#endif // COMBAT_H