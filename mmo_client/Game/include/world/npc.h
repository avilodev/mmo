#ifndef NPC_H
#define NPC_H

#include "core/game_types.h"

// ============================================================================
// NPC MODULE
// Handles NPC rendering and health bars
// ============================================================================

// Render all visible NPCs
void npc_render_all(const VisibleNPC* npcs, int count, int tile_size);

// Render single NPC
void npc_render(const VisibleNPC* npc, int tile_size);

// Find NPC by ID, returns NULL if not found
const VisibleNPC* npc_find_by_id(const VisibleNPC* npcs, int count, uint32_t npc_id);

// Get NPC position by ID, returns 0 if not found
int npc_get_position(const VisibleNPC* npcs, int count, uint32_t npc_id, 
                     float* out_x, float* out_y);

#endif // NPC_H