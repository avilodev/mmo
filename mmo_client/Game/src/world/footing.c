// Footing map loader; the file layout is in Game/tools/footing_math.py

#include "world/footing.h"
#include "core/client_log.h"
#include "world_regions.h"
#include "world_spans.h"
#include "world_spans_format.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FILE_MAGIC          "CFTG"
#define FILE_VERSION        4u
#define MAX_GRID_SIDE       20000
#define MAX_CELLS_PER_TILE  64
#define MAX_LEVELS_PER_TILE 8
#define MAX_LEVELS          400000u
#define MAX_COORD           1.0e7f
#define MAX_TILE_OFFSET     1000000
#define HEADER_BYTES        32u
#define DELTA_UNIT          0.25f
#define DELTA_NONE          255

typedef struct {
	int32_t min_x;
	int32_t min_y;
	int32_t width;
	int32_t height;
	int32_t cells;
	uint32_t levels;
} footing_header;

static int g_width = 0;
static int g_height = 0;
static int g_min_x = 0;
static int g_min_y = 0;
static int g_cells = 0;
static uint8_t* g_count = NULL;
static uint32_t* g_first = NULL;
static float* g_base = NULL;
static uint8_t* g_delta = NULL;

void footing_free(void) {
	free(g_count);
	free(g_first);
	free(g_base);
	free(g_delta);
	g_count = NULL;
	g_first = NULL;
	g_base = NULL;
	g_delta = NULL;
	g_width = 0;
	g_height = 0;
	g_min_x = 0;
	g_min_y = 0;
	g_cells = 0;
}

int footing_loaded(void) {
	return g_count != NULL;
}

static int read_u32(FILE* f, uint32_t* out) {
	uint8_t b[4];

	if(fread(b, 1, 4, f) != 4)
		return 0;
	*out = (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) |
	       ((uint32_t)b[3] << 24);

	return 1;
}

static int read_header(FILE* f, footing_header* h) {
	char magic[4];
	uint32_t version = 0;
	uint32_t v[5] = { 0 };

	if(fread(magic, 1, 4, f) != 4 || memcmp(magic, FILE_MAGIC, 4) != 0)
		return 0;
	if(!read_u32(f, &version) || version != FILE_VERSION)
		return 0;
	for(int i = 0; i < 5; i++)
		if(!read_u32(f, &v[i]))
			return 0;
	if(!read_u32(f, &h->levels))
		return 0;
	h->min_x = (int32_t)v[0];
	h->min_y = (int32_t)v[1];
	h->width = (int32_t)v[2];
	h->height = (int32_t)v[3];
	h->cells = (int32_t)v[4];

	return h->width > 0 && h->height > 0 && h->width <= MAX_GRID_SIDE &&
	       h->height <= MAX_GRID_SIDE && h->cells > 0 &&
	       h->cells <= MAX_CELLS_PER_TILE && h->levels <= MAX_LEVELS &&
	       abs(h->min_x) <= MAX_TILE_OFFSET && abs(h->min_y) <= MAX_TILE_OFFSET;
}

// Whether the file is big enough for what its header says, before allocating
static int holds_what_it_claims(FILE* f, const footing_header* h) {
	uint64_t tiles = (uint64_t)h->width * (uint64_t)h->height;
	uint64_t per = (uint64_t)h->cells * (uint64_t)h->cells;
	uint64_t need = HEADER_BYTES + tiles + (uint64_t)h->levels * (4u + 2u * per);
	long here = ftell(f);
	long size;

	if(here < 0 || fseek(f, 0, SEEK_END) != 0)
		return 0;
	size = ftell(f);
	if(fseek(f, here, SEEK_SET) != 0)
		return 0;

	return size >= 0 && (uint64_t)size >= need;
}

static int read_tiles(FILE* f, const footing_header* h) {
	size_t tiles = (size_t)h->width * (size_t)h->height;
	size_t per = (size_t)h->cells * (size_t)h->cells;
	uint32_t next = 0;

	g_count = calloc(tiles, 1);
	g_first = calloc(tiles, sizeof(uint32_t));
	g_base = malloc(((size_t)h->levels + 1) * sizeof(float));
	g_delta = malloc((size_t)h->levels * per + 1);
	if(!g_count || !g_first || !g_base || !g_delta)
		return 0;

	for(size_t t = 0; t < tiles; t++) {
		int n = fgetc(f);

		if(n == EOF || n > MAX_LEVELS_PER_TILE || next + (uint32_t)n > h->levels)
			return 0;
		g_count[t] = (uint8_t)n;
		g_first[t] = next;
		for(int k = 0; k < n; k++) {
			uint32_t bits = 0;

			if(!read_u32(f, &bits))
				return 0;
			memcpy(&g_base[next], &bits, sizeof(float));
			if(!isfinite(g_base[next]))
				return 0;
			if(fread(&g_delta[(size_t)next * per], 1, per, f) != per)
				return 0;
			// the room above each floor is for collision, not for the feet
			if(fseek(f, (long)per, SEEK_CUR) != 0)
				return 0;
			next++;
		}
	}

	return next == h->levels;
}

int footing_load(const char* path) {
	footing_header h = { 0 };
	FILE* f;
	int ok;

	footing_free();
	f = fopen(path, "rb");
	if(!f) {
		CLOG_ERROR("[FOOTING] Cannot open %s", path);
		return 0;
	}
	ok = read_header(f, &h) && holds_what_it_claims(f, &h) && read_tiles(f, &h);
	fclose(f);
	if(!ok) {
		CLOG_ERROR("[FOOTING] %s is not a valid footing file", path);
		footing_free();
		return 0;
	}
	g_width = h.width;
	g_height = h.height;
	g_min_x = h.min_x;
	g_min_y = h.min_y;
	g_cells = h.cells;
	CLOG_INFO("[FOOTING] %u levels over %d x %d tiles", h.levels, h.width, h.height);

	return 1;
}

static int floor_div(int a, int b) {
	int q = a / b;

	if(a % b != 0 && ((a < 0) != (b < 0)))
		q--;

	return q;
}

// Table index of the fine tile holding cell (gx, gy); -1 if flat or outside
static long fine_tile(int gx, int gy) {
	int col = floor_div(gx, g_cells) - CITY_ENNARA_TILE_X - g_min_x;
	int row = floor_div(gy, g_cells) - CITY_ENNARA_TILE_Y - g_min_y;
	size_t t;

	if(col < 0 || col >= g_width || row < 0 || row >= g_height)
		return -1;
	t = (size_t)row * (size_t)g_width + (size_t)col;

	return g_count[t] ? (long)t : -1;
}

// The span floor nearest the hint, skipping any more than a step above it
static int span_floor(float x, float y, float hint_z, float* out) {
	world_span spans[WORLD_SPANS_MAX_PER_TILE];
	int count = world_spans_at(x, y, spans, WORLD_SPANS_MAX_PER_TILE);
	int found = 0;

	for(int i = 0; i < count; i++) {
		if(spans[i].ceil - spans[i].floor < WORLD_BODY_HEIGHT ||
		   spans[i].floor > hint_z + WORLD_STEP_HEIGHT)
			continue;
		if(!found || fabsf(spans[i].floor - hint_z) < fabsf(*out - hint_z)) {
			*out = spans[i].floor;
			found = 1;
		}
	}

	return found;
}

// The nearest-to-hint level height of cell (ci, cj) of fine tile t
static int level_floor(long t, int ci, int cj, float hint_z, float* out) {
	size_t per = (size_t)g_cells * (size_t)g_cells;
	int found = 0;

	for(int k = 0; k < g_count[t]; k++) {
		size_t level = (size_t)g_first[t] + (size_t)k;
		uint8_t delta = g_delta[level * per + (size_t)cj * (size_t)g_cells + (size_t)ci];
		float z = g_base[level] + (float)delta * DELTA_UNIT;

		if(delta == DELTA_NONE || z > hint_z + WORLD_STEP_HEIGHT)
			continue;
		if(!found || fabsf(z - hint_z) < fabsf(*out - hint_z)) {
			*out = z;
			found = 1;
		}
	}

	return found;
}

// The floor of fine cell (gx, gy), from its tile's levels or else the spans
static int cell_floor(int gx, int gy, float hint_z, float* out) {
	float size = (float)WORLD_TILE_PX / (float)g_cells;
	long t = fine_tile(gx, gy);
	int ci = gx - floor_div(gx, g_cells) * g_cells;
	int cj = gy - floor_div(gy, g_cells) * g_cells;

	if(t >= 0 && level_floor(t, ci, cj, hint_z, out))
		return 1;

	return span_floor(((float)gx + 0.5f) * size, ((float)gy + 0.5f) * size, hint_z, out);
}

int footing_height_at(float x, float y, float hint_z, float* out_z) {
	float size;
	float u;
	float v;
	float fu;
	float fv;
	float center = 0.0f;
	float sum = 0.0f;
	int cx;
	int cy;
	int gx;
	int gy;

	if(!isfinite(x) || !isfinite(y) || !isfinite(hint_z))
		return 0;
	if(fabsf(x) > MAX_COORD || fabsf(y) > MAX_COORD)
		return 0;
	if(g_cells <= 0)
		return span_floor(x, y, hint_z, out_z);

	size = (float)WORLD_TILE_PX / (float)g_cells;
	cx = (int)floorf(x / size);
	cy = (int)floorf(y / size);
	if(!cell_floor(cx, cy, hint_z, &center))
		return 0;
	if(fine_tile(cx, cy) < 0) {
		*out_z = center;
		return 1;
	}

	// bilinear between cell centres, except across an edge
	u = x / size - 0.5f;
	v = y / size - 0.5f;
	gx = (int)floorf(u);
	gy = (int)floorf(v);
	fu = u - (float)gx;
	fv = v - (float)gy;
	for(int j = 0; j < 2; j++) {
		for(int i = 0; i < 2; i++) {
			float h;
			float weight = (i ? fu : 1.0f - fu) * (j ? fv : 1.0f - fv);

			if(!cell_floor(gx + i, gy + j, hint_z, &h) ||
			   fabsf(h - center) > FOOTING_EDGE_UNITS)
				h = center;
			sum += weight * h;
		}
	}
	*out_z = sum;

	return 1;
}

// The rise per unit of run along (dx, dy) through a point at height h0
static float rise_along(float x, float y, float h0, float dx, float dy, float reach) {
	float ahead = h0;
	float behind = h0;
	float limit = reach + FOOTING_SLOPE_SLACK;

	footing_height_at(x + dx * reach, y + dy * reach, h0, &ahead);
	footing_height_at(x - dx * reach, y - dy * reach, h0, &behind);
	// a sample across a step is not part of the slope
	if(fabsf(ahead - h0) > limit)
		ahead = h0;
	if(fabsf(behind - h0) > limit)
		behind = h0;

	return (ahead - behind) / (2.0f * reach);
}

void footing_slope_at(float x, float y, float hint_z, float dir_x, float dir_y,
                      float reach, float* along, float* across) {
	float h0 = 0.0f;

	*along = 0.0f;
	*across = 0.0f;
	if(!(reach > 0.0f) || !footing_height_at(x, y, hint_z, &h0))
		return;
	*along = rise_along(x, y, h0, dir_x, dir_y, reach);
	*across = rise_along(x, y, h0, -dir_y, dir_x, reach);
}
