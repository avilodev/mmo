#ifndef WEATHER_DRAWER_VIEW_H
#define WEATHER_DRAWER_VIEW_H

// Where the weather drawer's map is looking, and moving it about. No GL.

#include "ui/map_layout.h"
#include "ui/map_terrain.h"
#include "ui/weather_layout.h"

/** The footprint of the city in world units. */
typedef struct {
	float min_x;
	float min_y;
	float max_x;
	float max_y;
} weather_bounds;

/** Where the map looks: zoom 1 shows the whole footprint in the box. */
typedef struct {
	float zoom;
	float cam_x;   // the world point at the middle of the box
	float cam_y;
} weather_cam;

/** Zoom 1, the footprint's middle. */
weather_cam weather_cam_whole(const weather_bounds* bounds);

/** Screen units per world unit at a zoom; 0 when the footprint has no size. */
float weather_drawer_k(const weather_bounds* bounds, const weather_rect* inner, float zoom);

/** The most the map zooms in: as close as the full map goes. Never below 1. */
float weather_drawer_zoom_max(const weather_bounds* bounds, const weather_rect* inner);

/** Keep the zoom in its range and the middle of the view near the city. */
void weather_drawer_clamp(const weather_bounds* bounds, const weather_rect* inner,
                          weather_cam* cam);

/** Zoom by a factor, keeping the world point under (sx, sy) where it is. */
void weather_drawer_zoom_at(const weather_bounds* bounds, const weather_rect* inner,
                            weather_cam* cam, float factor, float sx, float sy);

/** Drag the map by a mouse movement in screen units. */
void weather_drawer_pan(const weather_bounds* bounds, const weather_rect* inner,
                        weather_cam* cam, float dx, float dy);

/** What the box shows: the terrain to draw and how the world maps onto the box.
 *  The image is the box's size in pixels (px_per_unit of them to a unit), and at
 *  most max_px on its longer side, however big the world is.
 *  @return Nonzero when the footprint has a size and the view was filled. */
int weather_drawer_view(const weather_bounds* bounds, const weather_rect* inner,
                        const weather_cam* cam, float px_per_unit, int max_px,
                        map_terrain_view* view, map_projection* proj);

#endif /* WEATHER_DRAWER_VIEW_H */
