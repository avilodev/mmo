// ============================================================================
// combat.h — public API for the new combat system
// ============================================================================

#ifndef COMBAT_H
#define COMBAT_H

#include "combat_config.h"
#include "types.h"         
#include "player_data.h"   

#include <stdio.h>
#include <stdint.h>

// Load per-class attack profiles from JSON (call before accepting clients).
// Returns the number of profiles loaded, or 0 on failure (compiled defaults remain).
int combat_profiles_load(const char* path);

// Initialize the NPC world (call once at server startup)
void combat_npc_init(NPCWorld* world);

// Spawn an NPC into the world. Returns the assigned ID, or 0 on failure.
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

// Look up an NPC by ID. Returns pointer or NULL. Caller must hold world->lock.
NPCEntity* combat_npc_find(NPCWorld* world, uint32_t npc_id);

// Remove a dead NPC (or despawn). Pass the NPC's ID.
void combat_npc_remove(NPCWorld* world, uint32_t npc_id);

// Handle an incoming ATTACK_INTENT from a player.
// Validates cooldown/cast state, snapshots the cast, and either sends
// CAST_START_V2 (if targets were found) or ATTACK_RESULT (if rejected).
void combat_handle_attack_intent(NPCWorld* world,
                                 int client_fd,
                                 uint32_t attacker_id,
                                 AttackIntentPacket* pkt);

// Handle an incoming CAST_CANCEL from a player.
// Clears the pending cast if one is active; sends CAST_CANCEL to client.
void combat_handle_cast_cancel(int client_fd, uint32_t attacker_id);

// Per-tick combat update. Call this from your combat_update_thread at whatever
// tick rate you like (e.g. 20 Hz). It checks all active casts, resolves any
// whose duration has elapsed, deals damage, and broadcasts DAMAGE_V2.
void combat_tick(NPCWorld* world);

#endif // COMBAT_H