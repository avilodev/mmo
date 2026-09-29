#ifndef NPC_H
#define NPC_H

#include "core/game_types.h"

/**
 * @file
 * Declare visible-NPC lookup, positioning, rendering, and targeting helpers.
 *
 * An NPC is drawn in three passes, because each belongs at a different point
 * in the frame: marks on the ground (under everything that stands), the 3D
 * body (depth-tested against buildings), and the label card above its head.
 */

/** Height of an NPC's body, in world units: the character height scaled by
 *  its type's size. Drawing and click-testing both use it, so they agree. */
float npc_body_height(const VisibleNPC* npc);

/** Shadow under each NPC, the selection ring under the targeted NPC and the
 *  elite ring under elites. Draw in the ground pass (camera applied, before
 *  structures). */
void npc_render_ground_marks(const VisibleNPC* npcs, int count, uint32_t target_npc_id);

/** Draw every living NPC's 3D body. Call between character_renderer_begin()
 *  and character_renderer_end().
 *
 * @param player_x, player_y  The local player: quest givers turn to face them.
 */
void npc_render_bodies(const VisibleNPC* npcs, int count, float player_x, float player_y);

/** Draw every living NPC's health bar, name and quest badge above its head.
 *
 * @param tracked_npc_type  The NPC type the tracked quest step points at, or 0
 *                          for none. Matching NPCs get the objective badge; the
 *                          type is passed in rather than the quest log so the
 *                          world renderer stays independent of quest structures.
 */
void npc_render_labels(const VisibleNPC* npcs, int count, uint32_t tracked_npc_type);

const VisibleNPC* npc_find_by_id(const VisibleNPC* npcs, int count, uint32_t npc_id);

int npc_get_position(const VisibleNPC* npcs, int count, uint32_t npc_id,
                     float* out_x, float* out_y);

#endif // NPC_H
