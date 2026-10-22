// Move the weather drawer's map about: zoom, drag and what it shows.

#include "ui/weather_drawer_view.h"
#include "core/game_types.h"

#include <math.h>

// Room left round the footprint at zoom 1, as a share of its size
#define MARGIN 1.04f

// How far the middle of the view may leave the footprint, as a share of its size
#define PAN_MARGIN 0.12f
#define MIN_PX 16

static float width_of(const weather_bounds* b) {
	return b->max_x - b->min_x;
}

static float height_of(const weather_bounds* b) {
	return b->max_y - b->min_y;
}

weather_cam weather_cam_whole(const weather_bounds* bounds) {
	weather_cam cam = { 1.0f, (bounds->min_x + bounds->max_x) * 0.5f,
	                    (bounds->min_y + bounds->max_y) * 0.5f };

	return cam;
}

// Screen units per world unit with the whole footprint in the box
static float whole_k(const weather_bounds* b, const weather_rect* inner) {
	float world_w = width_of(b) * MARGIN;
	float world_h = height_of(b) * MARGIN;
	float k;

	if(world_w <= 0.0f || world_h <= 0.0f)
		return 0.0f;

	k = inner->w / world_w;
	if(inner->h / world_h < k)
		k = inner->h / world_h;

	return k;
}

float weather_drawer_k(const weather_bounds* bounds, const weather_rect* inner, float zoom) {
	return whole_k(bounds, inner) * zoom;
}

float weather_drawer_zoom_max(const weather_bounds* bounds, const weather_rect* inner) {
	float whole = whole_k(bounds, inner);
	float closest = (inner->w * 0.5f) / map_zoom_half_width(MAP_ZOOM_MAX);

	if(whole <= 0.0f || closest <= whole)
		return 1.0f;

	return closest / whole;
}

void weather_drawer_clamp(const weather_bounds* bounds, const weather_rect* inner,
                          weather_cam* cam) {
	float room_x = width_of(bounds) * PAN_MARGIN;
	float room_y = height_of(bounds) * PAN_MARGIN;
	float most = weather_drawer_zoom_max(bounds, inner);

	if(cam->zoom > most)
		cam->zoom = most;
	if(cam->zoom < 1.0f)
		cam->zoom = 1.0f;

	cam->cam_x = fminf(fmaxf(cam->cam_x, bounds->min_x - room_x), bounds->max_x + room_x);
	cam->cam_y = fminf(fmaxf(cam->cam_y, bounds->min_y - room_y), bounds->max_y + room_y);
}

void weather_drawer_zoom_at(const weather_bounds* bounds, const weather_rect* inner,
                            weather_cam* cam, float factor, float sx, float sy) {
	float centre_x = inner->x + inner->w * 0.5f;
	float centre_y = inner->y + inner->h * 0.5f;
	float old_k = weather_drawer_k(bounds, inner, cam->zoom);
	float new_k;
	float world_x;
	float world_y;

	if(old_k <= 0.0f)
		return;

	world_x = cam->cam_x + (sx - centre_x) / old_k;
	world_y = cam->cam_y + (sy - centre_y) / old_k;

	cam->zoom *= factor;
	weather_drawer_clamp(bounds, inner, cam);

	new_k = weather_drawer_k(bounds, inner, cam->zoom);
	cam->cam_x = world_x - (sx - centre_x) / new_k;
	cam->cam_y = world_y - (sy - centre_y) / new_k;
	weather_drawer_clamp(bounds, inner, cam);
}

void weather_drawer_pan(const weather_bounds* bounds, const weather_rect* inner,
                        weather_cam* cam, float dx, float dy) {
	float k = weather_drawer_k(bounds, inner, cam->zoom);

	if(k <= 0.0f)
		return;

	cam->cam_x -= dx / k;
	cam->cam_y -= dy / k;
	weather_drawer_clamp(bounds, inner, cam);
}

int weather_drawer_view(const weather_bounds* bounds, const weather_rect* inner,
                        const weather_cam* cam, float px_per_unit, int max_px,
                        map_terrain_view* view, map_projection* proj) {
	float k = weather_drawer_k(bounds, inner, cam->zoom);
	float width = inner->w * px_per_unit;
	float height = inner->h * px_per_unit;
	float longer = width > height ? width : height;

	if(k <= 0.0f || longer <= 0.0f)
		return 0;

	if(longer > (float)max_px) {
		width *= (float)max_px / longer;
		height *= (float)max_px / longer;
	}

	view->center_x = cam->cam_x;
	view->center_y = cam->cam_y;
	view->half_w = inner->w * 0.5f / k;
	view->half_h = inner->h * 0.5f / k;
	view->clip_y = MAP_TERRAIN_NO_CLIP;
	view->width = (int)(width + 0.5f);
	view->height = (int)(height + 0.5f);
	if(view->width < MIN_PX)
		view->width = MIN_PX;
	if(view->height < MIN_PX)
		view->height = MIN_PX;

	proj->area = (map_rect){ inner->x, inner->y, inner->w, inner->h };
	proj->cam_x = cam->cam_x;
	proj->cam_y = cam->cam_y;
	proj->k = k;

	return 1;
}
