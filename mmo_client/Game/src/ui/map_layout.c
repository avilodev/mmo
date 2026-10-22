/**
 * @file
 * Arrange the map screen and convert between world and screen points.
 */
#include "ui/map_layout.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define PANEL_MARGIN  20.0f
#define PANEL_MAX_W   1800.0f
#define PANEL_MAX_H   1000.0f
#define TITLE_H       42.0f
#define STATUS_H      32.0f
#define SIDE_W        350.0f
#define GUTTER        8.0f

void map_layout_compute(float viewport_w, float viewport_h, map_layout* out) {
	float pw = viewport_w - PANEL_MARGIN * 2.0f;
	float ph = viewport_h - PANEL_MARGIN * 2.0f;

	if(pw > PANEL_MAX_W)
		pw = PANEL_MAX_W;
	if(ph > PANEL_MAX_H)
		ph = PANEL_MAX_H;

	out->panel.x = floorf((viewport_w - pw) * 0.5f);
	out->panel.y = floorf((viewport_h - ph) * 0.5f);
	out->panel.w = pw;
	out->panel.h = ph;

	out->title = (map_rect){ out->panel.x, out->panel.y, pw, TITLE_H };
	out->status = (map_rect){ out->panel.x, out->panel.y + ph - STATUS_H, pw, STATUS_H };
	out->side = (map_rect){ out->panel.x + pw - SIDE_W - GUTTER,
	                        out->panel.y + TITLE_H + GUTTER, SIDE_W,
	                        ph - TITLE_H - STATUS_H - GUTTER * 2.0f };
	out->area = (map_rect){ out->panel.x + GUTTER, out->panel.y + TITLE_H + GUTTER,
	                        pw - SIDE_W - GUTTER * 3.0f,
	                        ph - TITLE_H - STATUS_H - GUTTER * 2.0f };
}

float map_zoom_half_width(float zoom) {
	return MAP_BASE_RADIUS / zoom;
}

void map_projection_make(map_projection* out, const map_rect* area,
                         float cam_x, float cam_y, float zoom) {
	out->area = *area;
	out->cam_x = cam_x;
	out->cam_y = cam_y;
	out->k = (area->w * 0.5f) / map_zoom_half_width(zoom);
}

void map_project(const map_projection* p, float world_x, float world_y,
                 float* screen_x, float* screen_y) {
	*screen_x = p->area.x + p->area.w * 0.5f + (world_x - p->cam_x) * p->k;
	*screen_y = p->area.y + p->area.h * 0.5f + (world_y - p->cam_y) * p->k;
}

void map_unproject(const map_projection* p, float screen_x, float screen_y,
                   float* world_x, float* world_y) {
	*world_x = p->cam_x + (screen_x - p->area.x - p->area.w * 0.5f) / p->k;
	*world_y = p->cam_y + (screen_y - p->area.y - p->area.h * 0.5f) / p->k;
}

int map_rect_has(const map_rect* r, float x, float y) {
	return x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h;
}

float map_metres(float world_units) {
	return world_units / CITY_UNITS_PER_METRE;
}

void map_describe_offset(char* out, int out_size, float dx, float dy, float dz) {
	float ground = map_metres(sqrtf(dx * dx + dy * dy));
	float rise = map_metres(dz);

	if(ground < 1.0f)
		snprintf(out, (size_t)out_size, "here");
	else
		snprintf(out, (size_t)out_size, "%.0f m", ground);
	if(fabsf(rise) >= 2.0f) {
		size_t used = strlen(out);

		snprintf(out + used, (size_t)out_size - used, ", %+.0f m %s", rise,
		         rise > 0.0f ? "up" : "down");
	}
}
