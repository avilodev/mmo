#ifndef MAP_GRID_H
#define MAP_GRID_H

// The map's measuring marks: grid, scale bar and compass.

#include "ui/map_layout.h"

/** The grid spacing, in metres, that keeps lines readable at this zoom. */
float map_grid_step_metres(const map_projection* proj);

/** Draw the grid over the area, labelled along its top and left edges in
 *  metres east/west and north/south of the courtyard centre. */
void map_grid_draw(const map_projection* proj);

/** Draw a scale bar in the area's lower left corner. */
void map_scale_bar_draw(const map_projection* proj);

/** Draw a compass rose in the area's upper right corner. */
void map_compass_draw(const map_rect* area);

#endif /* MAP_GRID_H */
