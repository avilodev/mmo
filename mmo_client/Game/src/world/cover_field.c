// The heights of the highest surfaces over the city.

#include "world/cover_field.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static float* g_heights;
static int g_width;
static int g_height;
static float g_x0;
static float g_y0;
static float g_cell_w;
static float g_cell_h;

void cover_field_clear(void) {
	free(g_heights);
	g_heights = NULL;
	g_width = 0;
	g_height = 0;
}

void cover_field_set(const float* heights, int width, int height, float x0, float y0,
                     float cell_w, float cell_h) {
	size_t bytes = (size_t)width * (size_t)height * sizeof(float);

	cover_field_clear();
	if(width <= 0 || height <= 0 || cell_w <= 0.0f || cell_h <= 0.0f)
		return;
	g_heights = malloc(bytes);
	if(!g_heights)
		return;
	memcpy(g_heights, heights, bytes);
	g_width = width;
	g_height = height;
	g_x0 = x0;
	g_y0 = y0;
	g_cell_w = cell_w;
	g_cell_h = cell_h;
}

int cover_field_height(float x, float y, float* out) {
	int cx;
	int cy;

	if(!g_heights)
		return 0;
	cx = (int)floorf((x - g_x0) / g_cell_w);
	cy = (int)floorf((y - g_y0) / g_cell_h);
	if(cx < 0 || cy < 0 || cx >= g_width || cy >= g_height)
		return 0;
	*out = g_heights[cy * g_width + cx];

	return 1;
}

int cover_field_covered(float x, float y, float h, float clearance) {
	float top;

	if(!cover_field_height(x, y, &top))
		return 0;

	return top - h > clearance;
}
