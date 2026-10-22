#ifndef MAP_VIEW_H
#define MAP_VIEW_H

// The full map screen: the live city from above, with everything on it named.

#include "core/game_types.h"

/** Run one frame of map input while it is open: drag to pan, the re-centre
 *  button, and keeping the zoom inside what the city needs. Call every frame;
 *  with the map closed it puts the view back on the player. */
void map_view_update(GameState* game);

/** Draw the map screen when it is open. */
void map_view_render(GameState* game);

/** Release the terrain texture. */
void map_view_shutdown(void);

#endif /* MAP_VIEW_H */
