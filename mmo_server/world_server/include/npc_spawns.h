#ifndef NPC_SPAWNS_H
#define NPC_SPAWNS_H

#include "combat_config.h"

// Load NPC spawns from a JSON file and spawn them into the world.
// Returns the number of NPCs spawned, or -1 on error.
int npc_spawns_load(const char* json_filepath, NPCWorld* world);

#endif // NPC_SPAWNS_H
