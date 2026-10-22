/**
 * @file
 * Draw the map's grid, scale bar and compass.
 */
#include "ui/map_grid.h"
#include "ui/map_draw.h"
#include "render/renderer.h"

#include <math.h>
#include <stdio.h>

#define MIN_LINE_GAP_PX 78.0f

static const float STEPS_M[] = { 2.0f, 5.0f, 10.0f, 20.0f, 50.0f, 100.0f, 200.0f,
                                 500.0f, 1000.0f, 2000.0f };

float map_grid_step_metres(const map_projection* proj) {
	float px_per_metre = proj->k * CITY_UNITS_PER_METRE;
	int count = (int)(sizeof(STEPS_M) / sizeof(STEPS_M[0]));

	for(int i = 0; i < count; i++)
		if(STEPS_M[i] * px_per_metre >= MIN_LINE_GAP_PX)
			return STEPS_M[i];

	return STEPS_M[count - 1];
}

// "20 E" or "10 W" for an offset along x; "10 N" or "20 S" along y.
static void edge_label(char* out, int size, float metres, int along_x) {
	int whole = (int)floorf(fabsf(metres) + 0.5f);

	if(whole == 0)
		snprintf(out, (size_t)size, "0");
	else if(along_x)
		snprintf(out, (size_t)size, "%d %s", whole, metres > 0.0f ? "E" : "W");
	else
		snprintf(out, (size_t)size, "%d %s", whole, metres > 0.0f ? "S" : "N");
}

// The line through the courtyard centre stands out from the rest.
static map_color line_color(int n) {
	if(n == 0)
		return (map_color){ 1.0f, 0.8f, 0.4f, 0.35f };

	return (map_color){ 1.0f, 1.0f, 1.0f, 0.1f };
}

static void draw_vertical_lines(const map_projection* proj, float step_units) {
	const map_rect* a = &proj->area;
	float left = proj->cam_x - a->w * 0.5f / proj->k - CITY_ORIGIN_X;
	int n = (int)ceilf(left / step_units);
	char text[24];

	for(;; n++) {
		float world_x = CITY_ORIGIN_X + (float)n * step_units;
		float sx = a->x + a->w * 0.5f + (world_x - proj->cam_x) * proj->k;

		if(sx > a->x + a->w)
			break;
		map_fill(&(map_rect){ sx, a->y, 1.0f, a->h }, line_color(n));
		edge_label(text, sizeof(text), map_metres((float)n * step_units), 1);
		map_text_shadowed(sx + 3.0f, a->y + 11.0f, MAP_TEXT_SMALL, text,
		                  (map_color){ 0.82f, 0.86f, 0.95f, 0.9f });
	}
}

static void draw_horizontal_lines(const map_projection* proj, float step_units) {
	const map_rect* a = &proj->area;
	float top = proj->cam_y - a->h * 0.5f / proj->k - CITY_ORIGIN_Y;
	int n = (int)ceilf(top / step_units);
	char text[24];

	for(;; n++) {
		float world_y = CITY_ORIGIN_Y + (float)n * step_units;
		float sy = a->y + a->h * 0.5f + (world_y - proj->cam_y) * proj->k;

		if(sy > a->y + a->h)
			break;
		map_fill(&(map_rect){ a->x, sy, a->w, 1.0f }, line_color(n));
		edge_label(text, sizeof(text), map_metres((float)n * step_units), 0);
		map_text_shadowed(a->x + 4.0f, sy - 3.0f, MAP_TEXT_SMALL, text,
		                  (map_color){ 0.82f, 0.86f, 0.95f, 0.9f });
	}
}

void map_grid_draw(const map_projection* proj) {
	float step_units = map_grid_step_metres(proj) * CITY_UNITS_PER_METRE;

	draw_vertical_lines(proj, step_units);
	draw_horizontal_lines(proj, step_units);
}

void map_scale_bar_draw(const map_projection* proj) {
	float metres = map_grid_step_metres(proj);
	float length = metres * CITY_UNITS_PER_METRE * proj->k;
	char text[24];

	snprintf(text, sizeof(text), "%.0f m", metres);

	float total = length + 12.0f + map_text_width(MAP_TEXT_BODY, text) + 8.0f;
	float x = proj->area.x + proj->area.w - total - 8.0f;
	float y = proj->area.y + proj->area.h - 18.0f;

	renderer_draw_rect(x - 6.0f, y - 18.0f, total, 30.0f, 0.0f, 0.0f, 0.0f, 0.6f);
	renderer_draw_rect(x, y, length, 3.0f, 1.0f, 1.0f, 1.0f, 0.95f);
	renderer_draw_rect(x, y - 5.0f, 2.0f, 13.0f, 1.0f, 1.0f, 1.0f, 0.95f);
	renderer_draw_rect(x + length - 2.0f, y - 5.0f, 2.0f, 13.0f, 1.0f, 1.0f, 1.0f, 0.95f);
	map_text(x + length + 8.0f, y + 5.0f, MAP_TEXT_BODY, text,
	         (map_color){ 1.0f, 1.0f, 1.0f, 1.0f });
}

void map_compass_draw(const map_rect* area) {
	float cx = area->x + area->w - 40.0f;
	float cy = area->y + 46.0f;
	map_color ink = { 0.92f, 0.94f, 1.0f, 1.0f };

	renderer_draw_circle(cx, cy, 31.0f, 0.0f, 0.0f, 0.0f, 0.55f, 28);
	renderer_draw_ring(cx, cy, 29.0f, 31.0f, 0.55f, 0.65f, 0.85f, 0.9f, 28);
	map_dart(cx, cy - 6.0f, 0.0f, -1.0f, 9.0f, (map_color){ 0.95f, 0.3f, 0.25f, 1.0f });
	renderer_draw_rect(cx - 0.5f, cy + 4.0f, 1.0f, 12.0f, 0.8f, 0.85f, 1.0f, 0.8f);
	map_text_shadowed(cx - 4.0f, cy - 17.0f, MAP_TEXT_BODY, "N", ink);
	map_text_shadowed(cx - 4.0f, cy + 27.0f, MAP_TEXT_SMALL, "S", ink);
	map_text_shadowed(cx + 18.0f, cy + 4.0f, MAP_TEXT_SMALL, "E", ink);
	map_text_shadowed(cx - 25.0f, cy + 4.0f, MAP_TEXT_SMALL, "W", ink);
}
