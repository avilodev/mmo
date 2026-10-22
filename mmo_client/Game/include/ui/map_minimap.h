#ifndef MAP_MINIMAP_H
#define MAP_MINIMAP_H

// The minimap: the few metres around the player, drawn from the same city
// render and the same marks as the full map.

#include "core/game_types.h"
#include "ui/map_layout.h"

/** How far the minimap sees to each side of the player, in metres. */
#define MINIMAP_RADIUS_M 10.0f

/** Draw the minimap's terrain and marks inside a square on screen. */
void map_minimap_render(const GameState* game, const map_rect* box);

/** Release the minimap's terrain texture. */
void map_minimap_release(void);

#endif /* MAP_MINIMAP_H */
