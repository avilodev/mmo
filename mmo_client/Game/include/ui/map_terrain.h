#ifndef MAP_TERRAIN_H
#define MAP_TERRAIN_H

// The map's ground: the live city drawn from straight above into a texture.

#include "ui/map_layout.h"

/** What one render of the map's terrain shows. */
typedef struct {
	float center_x;   // world point at the middle of the image
	float center_y;
	float half_w;     // half the width of the world shown, world units
	float half_h;     // half the height of the world shown, world units
	float clip_y;     // nothing above this height is drawn; huge keeps roofs
	int   width;      // image size in pixels
	int   height;
} map_terrain_view;

/** A framebuffer and the last view drawn into it. Zero it before first use;
 *  each place that shows the terrain (the map, the minimap) owns one. */
typedef struct {
	unsigned int     fbo;
	unsigned int     color;
	unsigned int     depth;
	int              width;
	int              height;
	map_terrain_view last;
	int              have_last;
} map_terrain_target;

/** Height meaning "cut nothing": the roofs stay on. */
#define MAP_TERRAIN_NO_CLIP 1.0e9f

/** Draw the view into the target, or reuse the last render if it matches.
 *  North is up. Needs the city loaded and a current GL context.
 *  @return The texture (row 0 at the bottom, as GL stores it), or 0 on failure. */
unsigned int map_terrain_render(map_terrain_target* target, const map_terrain_view* view);

/** Render what a map projection shows, sized for the screen it is drawn on.
 *  viewport_w is the logical width the projection's pixels are measured in. */
unsigned int map_terrain_render_area(map_terrain_target* target, const map_projection* proj,
                                     float viewport_w, float clip_y);

/** Release the target's framebuffer and texture. */
void map_terrain_release(map_terrain_target* target);

#endif /* MAP_TERRAIN_H */
