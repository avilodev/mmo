#ifndef MAP_LAYOUT_H
#define MAP_LAYOUT_H

// Where the map's parts sit on screen, and how a world point becomes a pixel.

#include "render/city_renderer.h"

/** World units shown to each side of the map at zoom 1. */
#define MAP_BASE_RADIUS 1050.0f

/** One rectangle in logical screen pixels. */
typedef struct {
	float x, y, w, h;
} map_rect;

/** The map screen's parts. */
typedef struct {
	map_rect panel;   // the whole window
	map_rect title;   // the bar across the top
	map_rect area;    // the terrain and everything drawn on it
	map_rect side;    // the information column
	map_rect status;  // the readout along the bottom
} map_layout;

/** How the map area shows the world: north up, y down, as the world is. */
typedef struct {
	map_rect area;
	float    cam_x;   // the world point at the middle of the area
	float    cam_y;
	float    k;       // screen pixels per world unit
} map_projection;

/** Lay the screen out for a logical viewport. */
void map_layout_compute(float viewport_w, float viewport_h, map_layout* out);

/** Build the projection for a camera point and a zoom (1 shows MAP_BASE_RADIUS). */
void map_projection_make(map_projection* out, const map_rect* area,
                         float cam_x, float cam_y, float zoom);

void map_project(const map_projection* p, float world_x, float world_y,
                 float* screen_x, float* screen_y);
void map_unproject(const map_projection* p, float screen_x, float screen_y,
                   float* world_x, float* world_y);

/** Half the width of the world a zoom shows, in world units. */
float map_zoom_half_width(float zoom);

/** Whether a screen point lies inside a rectangle. */
int map_rect_has(const map_rect* r, float x, float y);

/** World units to metres. */
float map_metres(float world_units);

/** Write how far away an offset is, as "12 m", with ", +3 m up" when it is on
 *  another level. dx and dy are world units on the ground, dz the height. */
void map_describe_offset(char* out, int out_size, float dx, float dy, float dz);

#endif /* MAP_LAYOUT_H */
