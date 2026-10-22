#ifndef MAP_CUT_H
#define MAP_CUT_H

// Where the map cuts the city: roofs on outdoors, a floor plan under a ceiling.

#include "core/game_types.h"

/** The cut the terrain is drawn with. */
typedef struct {
	int   indoors;     // standing under a ceiling, so drawn as a floor plan
	float cut_height;  // the height of the cut, world units; huge when not indoors
} map_cut_info;

/** Work out the cut for where the player stands. */
map_cut_info map_cut_compute(const GameState* game);

#endif /* MAP_CUT_H */
