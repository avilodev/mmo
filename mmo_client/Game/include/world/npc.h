#ifndef NPC_H
#define NPC_H

#include "core/game_types.h"

/**
 * @file
 * Declare visible-NPC lookup, positioning, rendering, and targeting helpers.
 */

void npc_render_all(const VisibleNPC* npcs, int count, int tile_size);

void npc_render(const VisibleNPC* npc, int tile_size);

const VisibleNPC* npc_find_by_id(const VisibleNPC* npcs, int count, uint32_t npc_id);

int npc_get_position(const VisibleNPC* npcs, int count, uint32_t npc_id,
                     float* out_x, float* out_y);

void npc_render_target_indicator(const VisibleNPC* npcs, int count, int tile_size,
                                  uint32_t target_npc_id);

#endif // NPC_H
