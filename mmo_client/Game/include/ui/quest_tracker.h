#ifndef QUEST_TRACKER_H
#define QUEST_TRACKER_H

/**
 * @file
 * Draw the tracked quest's next step, and point at it in the world and on the map.
 *
 * All three surfaces read the same tracked step out of the quest log, so they
 * cannot disagree about what the player is supposed to do next.
 */

#include "ui/quest_log.h"

/** Draw the tracked step's panel in screen space, under the minimap. */
void quest_tracker_render_panel(const QuestLogState* ql, int vw, int vh);

/** Draw the badge above a tracked NPC, in world space.
 *
 * Called per visible NPC from the world render pass.
 *
 * @param top_y  World y of the space just above the NPC's name label.
 */
void quest_tracker_render_world_badge(float x, float top_y);

/** Draw the tracked step's marker on the open map.
 *
 * A marker outside the visible area is clamped to the edge and drawn as an
 * arrow, so a target on the far side of the city still says which way to walk.
 *
 * @param map_x, map_y, map_w, map_h  The map's drawing area in screen space.
 * @param centre_x, centre_y          Screen position of the player, the map's centre.
 * @param player_wx, player_wy        The player's world position.
 * @param scale                       Screen pixels per world pixel.
 */
void quest_tracker_render_map_marker(const QuestLogState* ql,
                                     float map_x, float map_y, float map_w, float map_h,
                                     float centre_x, float centre_y,
                                     float player_wx, float player_wy, float scale);

#endif // QUEST_TRACKER_H
