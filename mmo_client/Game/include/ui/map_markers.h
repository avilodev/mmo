#ifndef MAP_MARKERS_H
#define MAP_MARKERS_H

// Draw the map's marks: things standing on it, and what is happening to them.

#include "ui/map_entities.h"
#include "ui/map_layout.h"
#include "ui/map_pointer.h"

/** Start a frame of marks; clears the record of where labels went. */
void map_markers_begin(void);

/** Draw the zones, telegraphs and projectiles under everything else. */
void map_markers_draw_hazards(const GameState* game, const map_projection* proj);

/** Draw every entity. `hovered` is an index into the list, or -1. With `labels`
 *  zero no names are drawn (the minimap is too small to hold them). */
void map_markers_draw_entities(const map_entity_list* list, const map_projection* proj,
                               int hovered, int labels);

/** Draw the player's own mark, facing the way they face. The size is in the map's
 *  own units and does not change with the zoom. */
void map_markers_draw_self_sized(const GameState* game, const map_projection* proj, float size,
                                 map_pointer_style style);

/** The entity under a screen point, or -1. */
int map_markers_hit(const map_entity_list* list, const map_projection* proj,
                    float screen_x, float screen_y);

#endif /* MAP_MARKERS_H */
