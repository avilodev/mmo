#ifndef NPC_SPAWNS_H
#define NPC_SPAWNS_H

#include "npc_world.h"

// return the spawned NPC count or -1 when JSON loading fails
int npc_spawns_load(const char* json_filepath, NPCWorld* world);

#endif // NPC_SPAWNS_H
