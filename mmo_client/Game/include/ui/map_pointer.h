#ifndef MAP_POINTER_H
#define MAP_POINTER_H

// The mark for the player on a map: where they stand and which way they face.

#include "ui/map_draw.h"

typedef enum {
	MAP_POINTER_DART,    // the original wide yellow dart
	MAP_POINTER_ARROW,   // a notched navigation arrowhead
	MAP_POINTER_BEAM,    // a dot with a wedge of view in front of it
	MAP_POINTER_RING,    // a ring round a dot with a tick on the side faced
	MAP_POINTER_NEEDLE,  // a slim two-tone compass needle
	MAP_POINTER_COUNT
} map_pointer_style;

/** The mark the full map and the weather drawer use. */
#define MAP_POINTER_DEFAULT MAP_POINTER_ARROW

/** Draw a pointer centred on (cx, cy) facing (dir_x, dir_y), a unit vector in
 *  screen directions. `size` is the same unit the old dart used: 8 is a
 *  full-map pointer. */
void map_pointer_draw(map_pointer_style style, float cx, float cy, float dir_x, float dir_y,
                      float size, map_color color);

#endif /* MAP_POINTER_H */
