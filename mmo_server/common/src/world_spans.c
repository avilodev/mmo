// Span lookups over the sidecar beside world.dat. See world_spans_format.h.

#include "world_spans.h"
#include "world_regions.h"
#include "world_spans_format.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
	uint32_t tile_index;
	uint32_t first_span;
	uint32_t span_count;
} tile_record;

typedef struct {
	int32_t width;
	int32_t height;
	int32_t tile_size;
	uint32_t tile_count;
	uint32_t span_count;
} spans_header;

static int g_width = 0;
static int g_height = 0;
static float g_tile = 0.0f;
static tile_record* g_tiles = NULL;
static world_span* g_spans = NULL;
static uint32_t g_tile_count = 0;
static uint32_t g_span_count = 0;

static void free_tables(void) {
	free(g_tiles);
	free(g_spans);
	g_tiles = NULL;
	g_spans = NULL;
	g_tile_count = 0;
	g_span_count = 0;
}

static void free_mask(void);

void world_spans_init(int width_tiles, int height_tiles, int tile_size) {
	free_tables();
	free_mask();
	world_spans_set_mask_origin(CITY_ENNARA_TILE_X, CITY_ENNARA_TILE_Y);
	g_width = width_tiles;
	g_height = height_tiles;
	g_tile = (float)tile_size;
}

void world_spans_shutdown(void) {
	free_tables();
	free_mask();
	world_spans_set_mask_origin(CITY_ENNARA_TILE_X, CITY_ENNARA_TILE_Y);
	g_width = 0;
	g_height = 0;
	g_tile = 0.0f;
}

int world_spans_loaded(void) {
	return g_tiles != NULL;
}

// The tile holding a world point. Returns 0 when it is not inside the world.
static int tile_of(float x, float y, int* tx, int* ty) {
	if(g_width <= 0 || g_height <= 0 || !(g_tile > 0.0f))
		return 0;
	if(!isfinite(x) || !isfinite(y))
		return 0;

	float fx = x / g_tile;
	float fy = y / g_tile;

	if(!(fx >= 0.0f) || !(fy >= 0.0f))
		return 0;
	if(!(fx < (float)g_width) || !(fy < (float)g_height))
		return 0;

	*tx = (int)fx;
	*ty = (int)fy;
	return 1;
}

static const tile_record* find_tile(uint32_t index) {
	uint32_t lo = 0;
	uint32_t hi = g_tile_count;

	while(lo < hi) {
		uint32_t mid = lo + (hi - lo) / 2;

		if(g_tiles[mid].tile_index < index)
			lo = mid + 1;
		else
			hi = mid;
	}
	if(lo < g_tile_count && g_tiles[lo].tile_index == index)
		return &g_tiles[lo];
	return NULL;
}

// The fine floor mask, world_footing.bin (layout: Game/data/footing_math.py)
#define MASK_MAGIC        "CFTG"
#define MASK_VERSION      4u
#define MASK_HEADER_BYTES 32u
#define MASK_MAX_SIDE     20000
#define MASK_MAX_CELLS    64
#define MASK_MAX_LEVELS   400000u
#define MASK_MAX_OFFSET   1000000
#define MASK_ABSENT       255
#define MASK_OPEN         255
#define MASK_STEEP        254
#define MASK_CELL_UNIT    0.25f
#define MASK_CLEAR_UNIT   2.0f

static int g_mask_origin_x = CITY_ENNARA_TILE_X;
static int g_mask_origin_y = CITY_ENNARA_TILE_Y;
static int g_mask_min_x = 0;
static int g_mask_min_y = 0;
static int g_mask_w = 0;
static int g_mask_h = 0;
static int g_mask_cells = 0;
static size_t g_mask_per = 0;
static uint8_t* g_mask_count = NULL;
static uint32_t* g_mask_first = NULL;
static float* g_mask_base = NULL;
static uint8_t* g_mask_delta = NULL;
static uint8_t* g_mask_clear = NULL;

static void free_mask(void) {
	free(g_mask_count);
	free(g_mask_first);
	free(g_mask_base);
	free(g_mask_delta);
	free(g_mask_clear);
	g_mask_count = NULL;
	g_mask_first = NULL;
	g_mask_base = NULL;
	g_mask_delta = NULL;
	g_mask_clear = NULL;
	g_mask_w = 0;
	g_mask_h = 0;
	g_mask_cells = 0;
	g_mask_per = 0;
}

void world_spans_set_mask_origin(int tile_x, int tile_y) {
	g_mask_origin_x = tile_x;
	g_mask_origin_y = tile_y;
}

int world_spans_mask_loaded(void) {
	return g_mask_count != NULL;
}

typedef struct {
	int32_t min_x;
	int32_t min_y;
	int32_t width;
	int32_t height;
	int32_t cells;
	uint32_t levels;
} mask_header;

static int mask_read_u32(FILE* f, uint32_t* out) {
	uint8_t b[4];

	if(fread(b, 1, 4, f) != 4)
		return 0;
	*out = (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) |
	       ((uint32_t)b[3] << 24);
	return 1;
}

static int mask_read_header(FILE* f, mask_header* h) {
	char magic[4];
	uint32_t version = 0;
	uint32_t v[5] = { 0 };

	if(fread(magic, 1, 4, f) != 4 || memcmp(magic, MASK_MAGIC, 4) != 0)
		return 0;
	if(!mask_read_u32(f, &version) || version != MASK_VERSION)
		return 0;
	for(int i = 0; i < 5; i++)
		if(!mask_read_u32(f, &v[i]))
			return 0;
	if(!mask_read_u32(f, &h->levels))
		return 0;
	h->min_x = (int32_t)v[0];
	h->min_y = (int32_t)v[1];
	h->width = (int32_t)v[2];
	h->height = (int32_t)v[3];
	h->cells = (int32_t)v[4];

	return h->width > 0 && h->height > 0 && h->width <= MASK_MAX_SIDE &&
	       h->height <= MASK_MAX_SIDE && h->cells > 0 && h->cells <= MASK_MAX_CELLS &&
	       h->levels <= MASK_MAX_LEVELS && h->min_x >= -MASK_MAX_OFFSET &&
	       h->min_x <= MASK_MAX_OFFSET && h->min_y >= -MASK_MAX_OFFSET &&
	       h->min_y <= MASK_MAX_OFFSET;
}

// Is the file big enough for what its header says, before allocating?
static int mask_file_holds(FILE* f, const mask_header* h) {
	uint64_t tiles = (uint64_t)h->width * (uint64_t)h->height;
	uint64_t per = (uint64_t)h->cells * (uint64_t)h->cells;
	uint64_t need = MASK_HEADER_BYTES + tiles + (uint64_t)h->levels * (4u + 2u * per);
	long here = ftell(f);
	long size;

	if(here < 0 || fseek(f, 0, SEEK_END) != 0)
		return 0;
	size = ftell(f);
	if(fseek(f, here, SEEK_SET) != 0)
		return 0;

	return size >= 0 && (uint64_t)size >= need;
}

static int mask_read_tiles(FILE* f, const mask_header* h) {
	size_t tiles = (size_t)h->width * (size_t)h->height;
	size_t per = (size_t)h->cells * (size_t)h->cells;
	uint32_t next = 0;

	g_mask_count = calloc(tiles, 1);
	g_mask_first = calloc(tiles, sizeof(uint32_t));
	g_mask_base = calloc((size_t)h->levels + 1, sizeof(float));
	g_mask_delta = calloc(((size_t)h->levels + 1) * per, 1);
	g_mask_clear = calloc(((size_t)h->levels + 1) * per, 1);
	if(!g_mask_count || !g_mask_first || !g_mask_base || !g_mask_delta || !g_mask_clear)
		return 0;

	for(size_t t = 0; t < tiles; t++) {
		int n = fgetc(f);

		if(n == EOF || n > WORLD_SPANS_MAX_PER_TILE || next + (uint32_t)n > h->levels)
			return 0;
		g_mask_count[t] = (uint8_t)n;
		g_mask_first[t] = next;
		for(int k = 0; k < n; k++) {
			uint32_t bits = 0;

			if(!mask_read_u32(f, &bits))
				return 0;
			memcpy(&g_mask_base[next], &bits, sizeof(float));
			if(!isfinite(g_mask_base[next]))
				return 0;
			if(fread(g_mask_delta + (size_t)next * per, 1, per, f) != per ||
			   fread(g_mask_clear + (size_t)next * per, 1, per, f) != per)
				return 0;
			next++;
		}
	}
	g_mask_per = per;

	return next == h->levels;
}

int world_spans_load_mask(const char* path) {
	mask_header h = { 0 };
	FILE* f;
	int ok;

	free_mask();
	f = fopen(path, "rb");
	if(!f)
		return 0;
	ok = mask_read_header(f, &h) && mask_file_holds(f, &h) && mask_read_tiles(f, &h);
	fclose(f);
	if(!ok) {
		free_mask();
		return 0;
	}
	g_mask_min_x = h.min_x;
	g_mask_min_y = h.min_y;
	g_mask_w = h.width;
	g_mask_h = h.height;
	g_mask_cells = h.cells;

	return 1;
}

// First mask level of tile (tx, ty), or -1 if it has none for `spans` spans
static long mask_level_of(int tx, int ty, int spans) {
	int col = tx - g_mask_origin_x - g_mask_min_x;
	int row = ty - g_mask_origin_y - g_mask_min_y;
	size_t t;

	if(!g_mask_count || col < 0 || col >= g_mask_w || row < 0 || row >= g_mask_h)
		return -1;
	t = (size_t)row * (size_t)g_mask_w + (size_t)col;
	if(g_mask_count[t] == 0 || g_mask_count[t] != spans)
		return -1;

	return (long)g_mask_first[t];
}

// The cell index of a point inside tile (tx, ty)
static int mask_cell(float x, float y, int tx, int ty) {
	float size = g_tile / (float)g_mask_cells;
	int ci = (int)((x - (float)tx * g_tile) / size);
	int cj = (int)((y - (float)ty * g_tile) / size);

	if(ci < 0)
		ci = 0;
	if(ci >= g_mask_cells)
		ci = g_mask_cells - 1;
	if(cj < 0)
		cj = 0;
	if(cj >= g_mask_cells)
		cj = g_mask_cells - 1;

	return cj * g_mask_cells + ci;
}

// Drop the spans with no floor in the cell at (x, y); the rest take its floor
// and ceiling, and steep[i] says whether it is too steep to walk
static int mask_filter(float x, float y, int tx, int ty, world_span* out, uint8_t* steep,
                       int count, int total) {
	long first = mask_level_of(tx, ty, total);
	int cell = first >= 0 ? mask_cell(x, y, tx, ty) : 0;
	int kept = 0;

	if(first < 0)
		return count;
	for(int i = 0; i < count; i++) {
		size_t at = (size_t)(first + i) * g_mask_per + (size_t)cell;
		uint8_t delta = g_mask_delta[at];
		uint8_t clear = g_mask_clear[at];

		if(delta == MASK_ABSENT)
			continue;
		out[kept].floor = g_mask_base[first + i] + (float)delta * MASK_CELL_UNIT;
		if(clear == MASK_OPEN || clear == MASK_STEEP)
			out[kept].ceil = WORLD_SPAN_OPEN_CEIL;
		else
			out[kept].ceil = out[kept].floor + (float)clear * MASK_CLEAR_UNIT;
		steep[kept] = clear == MASK_STEEP;
		kept++;
	}

	return kept;
}

// The spans at (x, y), with which of them are steep
static int spans_at(float x, float y, world_span* out, uint8_t* steep, int max) {
	int tx = 0;
	int ty = 0;

	if(max < 1 || !tile_of(x, y, &tx, &ty))
		return 0;
	memset(steep, 0, (size_t)max);

	const tile_record* rec = find_tile((uint32_t)ty * (uint32_t)g_width + (uint32_t)tx);

	if(!rec) {
		out[0].floor = 0.0f;
		out[0].ceil = WORLD_SPAN_OPEN_CEIL;
		return 1;
	}

	int count = (int)rec->span_count;
	int total = count;

	if(count > max)
		count = max;
	if(count > 0)
		memcpy(out, &g_spans[rec->first_span], (size_t)count * sizeof(world_span));
	if(g_mask_count && count > 0)
		return mask_filter(x, y, tx, ty, out, steep, count, total);
	return count;
}

int world_spans_at(float x, float y, world_span* out, int max) {
	uint8_t steep[WORLD_SPANS_MAX_PER_TILE];

	if(max > WORLD_SPANS_MAX_PER_TILE)
		max = WORLD_SPANS_MAX_PER_TILE;
	return spans_at(x, y, out, steep, max);
}

static int span_usable(const world_span* s) {
	return s->ceil - s->floor >= WORLD_BODY_HEIGHT;
}

int world_spans_reach(float x, float y, float from_z, float* out_z) {
	world_span spans[WORLD_SPANS_MAX_PER_TILE];
	uint8_t steep[WORLD_SPANS_MAX_PER_TILE];
	int best = -1;
	float best_gap = 0.0f;

	if(!isfinite(from_z))
		return 0;

	int count = spans_at(x, y, spans, steep, WORLD_SPANS_MAX_PER_TILE);

	for(int i = 0; i < count; i++) {
		float gap = fabsf(spans[i].floor - from_z);

		if(!span_usable(&spans[i]) || gap > WORLD_STEP_HEIGHT)
			continue;
		// a steep floor is stood on or gone down, never climbed
		if(steep[i] && spans[i].floor > from_z + WORLD_STEEP_RISE)
			continue;
		if(best < 0 || gap < best_gap) {
			best = i;
			best_gap = gap;
		}
	}
	if(best < 0)
		return 0;
	if(out_z)
		*out_z = spans[best].floor;
	return 1;
}

// Does some span of this column hold a body standing at z?
static int column_holds(float x, float y, float z) {
	world_span spans[WORLD_SPANS_MAX_PER_TILE];
	int count = world_spans_at(x, y, spans, WORLD_SPANS_MAX_PER_TILE);

	for(int i = 0; i < count; i++) {
		if(fabsf(spans[i].floor - z) > WORLD_STEP_HEIGHT)
			continue;
		if(spans[i].ceil - z >= WORLD_BODY_HEIGHT)
			return 1;
	}
	return 0;
}

// Does some span of this column leave room for a body at z (floor anywhere
// at or below one step above z)?
static int column_free(float x, float y, float z) {
	world_span spans[WORLD_SPANS_MAX_PER_TILE];
	int count = world_spans_at(x, y, spans, WORLD_SPANS_MAX_PER_TILE);

	for(int i = 0; i < count; i++) {
		if(spans[i].floor > z + WORLD_STEP_HEIGHT)
			continue;
		if(spans[i].ceil - z >= WORLD_BODY_HEIGHT)
			return 1;
	}
	return 0;
}

// Does some span of this column hold a body at z with no floor above its feet?
static int column_clear(float x, float y, float z) {
	world_span spans[WORLD_SPANS_MAX_PER_TILE];
	int count = world_spans_at(x, y, spans, WORLD_SPANS_MAX_PER_TILE);

	for(int i = 0; i < count; i++) {
		if(spans[i].floor > z + WORLD_CLEAR_EPS)
			continue;
		if(spans[i].ceil - z >= WORLD_BODY_HEIGHT)
			return 1;
	}
	return 0;
}

// The box is tested at BOX_EDGE_POINTS evenly spaced points along each edge,
// corners included, so a solid wider than their spacing cannot slip between
#define BOX_EDGE_POINTS 4
#define BOX_POINTS (4 * (BOX_EDGE_POINTS - 1))

typedef int (*column_test)(float x, float y, float z);

static int box_points(float x, float y, float half, float* px, float* py) {
	float step = 2.0f * half / (float)(BOX_EDGE_POINTS - 1);
	int n = 0;

	for(int i = 0; i < BOX_EDGE_POINTS; i++) {
		float t = -half + step * (float)i;

		px[n] = x + t;
		py[n++] = y - half;
		px[n] = x + t;
		py[n++] = y + half;
	}
	for(int i = 1; i < BOX_EDGE_POINTS - 1; i++) {
		float t = -half + step * (float)i;

		px[n] = x - half;
		py[n++] = y + t;
		px[n] = x + half;
		py[n++] = y + t;
	}

	return n;
}

// Does every sample point of the box pass the column test at z?
static int box_all(float x, float y, float half, float z, column_test test) {
	float px[BOX_POINTS];
	float py[BOX_POINTS];
	int n;

	if(!isfinite(z) || !isfinite(half))
		return 0;
	n = box_points(x, y, half, px, py);
	for(int i = 0; i < n; i++) {
		if(!test(px[i], py[i], z))
			return 0;
	}

	return 1;
}

int world_spans_fits_box(float x, float y, float half, float z) {
	return box_all(x, y, half, z, column_holds);
}

int world_spans_free_box(float x, float y, float half, float z) {
	return box_all(x, y, half, z, column_free);
}

int world_spans_clear_box(float x, float y, float half, float z) {
	return box_all(x, y, half, z, column_clear);
}

int world_spans_reach_box(float x, float y, float half, float from_z,
                          float* out_z) {
	float z = 0.0f;

	if(!world_spans_reach(x, y, from_z, &z))
		return 0;
	if(!world_spans_fits_box(x, y, half, z))
		return 0;
	if(out_z)
		*out_z = z;
	return 1;
}

int world_spans_floor_between(float x, float y, float half, float z_lo,
                              float z_hi, float* out_z) {
	world_span spans[WORLD_SPANS_MAX_PER_TILE];
	int best = -1;

	if(!isfinite(z_lo) || !isfinite(z_hi))
		return 0;

	int count = world_spans_at(x, y, spans, WORLD_SPANS_MAX_PER_TILE);

	for(int i = 0; i < count; i++) {
		float f = spans[i].floor;

		if(!span_usable(&spans[i]) || f < z_lo || f > z_hi)
			continue;
		if(!world_spans_fits_box(x, y, half, f))
			continue;
		if(best < 0 || f > spans[best].floor)
			best = i;
	}
	if(best < 0)
		return 0;
	if(out_z)
		*out_z = spans[best].floor;
	return 1;
}

int world_spans_support(float x, float y, float half, float z_lo, float z_hi,
                        float* out_z) {
	float cx[BOX_POINTS];
	float cy[BOX_POINTS];
	float best = 0.0f;
	int found = 0;
	int n;

	if(!isfinite(x) || !isfinite(y) || !isfinite(half) || !isfinite(z_lo) ||
	   !isfinite(z_hi))
		return 0;
	n = box_points(x, y, half, cx, cy);

	for(int k = 0; k < n; k++) {
		world_span spans[WORLD_SPANS_MAX_PER_TILE];
		int count = world_spans_at(cx[k], cy[k], spans, WORLD_SPANS_MAX_PER_TILE);

		for(int i = 0; i < count; i++) {
			float f = spans[i].floor;

			if(!span_usable(&spans[i]) || f < z_lo || f > z_hi)
				continue;
			if(found && f <= best)
				continue;
			if(!world_spans_free_box(x, y, half, f))
				continue;
			best = f;
			found = 1;
		}
	}
	if(found && out_z)
		*out_z = best;

	return found;
}

int world_spans_steep_above(float x, float y, float z) {
	world_span spans[WORLD_SPANS_MAX_PER_TILE];
	uint8_t steep[WORLD_SPANS_MAX_PER_TILE];

	if(!isfinite(z))
		return 0;

	int count = spans_at(x, y, spans, steep, WORLD_SPANS_MAX_PER_TILE);

	for(int i = 0; i < count; i++) {
		if(steep[i] && spans[i].floor > z + WORLD_STEEP_RISE &&
		   spans[i].floor <= z + WORLD_STEP_HEIGHT)
			return 1;
	}
	return 0;
}

// The floor nearest z within a step in the column at (x, y); *steep_out says
// whether it is steep. 0 when there is none.
static int floor_near(float x, float y, float z, float* out, int* steep_out) {
	world_span spans[WORLD_SPANS_MAX_PER_TILE];
	uint8_t steep[WORLD_SPANS_MAX_PER_TILE];
	int count = spans_at(x, y, spans, steep, WORLD_SPANS_MAX_PER_TILE);
	int best = -1;

	for(int i = 0; i < count; i++) {
		if(!span_usable(&spans[i]) || fabsf(spans[i].floor - z) > WORLD_STEP_HEIGHT)
			continue;
		if(best < 0 || fabsf(spans[i].floor - z) < fabsf(spans[best].floor - z))
			best = i;
	}
	if(best < 0)
		return 0;
	*out = spans[best].floor;
	*steep_out = steep[best];

	return 1;
}

// How far either side of the feet the slope is sampled
#define SLIDE_PROBE 2.0f
// A slope under this grade is a lip, not a slope (45 degrees is 1.0): the body
// stands there, and does not slide, so it cannot be pushed off and walk back on
#define SLIDE_MIN_GRADE 0.75f

int world_spans_slide(float x, float y, float z, float* dir_x, float* dir_y, float* grade) {
	float at = 0.0f;
	float h[4];
	int steep = 0;
	float gx;
	float gy;
	float len;

	if(!isfinite(x) || !isfinite(y) || !isfinite(z))
		return 0;
	if(!floor_near(x, y, z, &at, &steep) || !steep || fabsf(at - z) > WORLD_STEEP_RISE)
		return 0;
	for(int k = 0; k < 4; k++) {
		float px = x + (k == 0 ? SLIDE_PROBE : k == 1 ? -SLIDE_PROBE : 0.0f);
		float py = y + (k == 2 ? SLIDE_PROBE : k == 3 ? -SLIDE_PROBE : 0.0f);
		int ignored = 0;

		if(!floor_near(px, py, at, &h[k], &ignored))
			h[k] = at;
	}
	gx = h[0] - h[1];
	gy = h[2] - h[3];
	len = sqrtf(gx * gx + gy * gy);
	if(!(len > 0.001f))
		return 0;
	if(len / (2.0f * SLIDE_PROBE) < SLIDE_MIN_GRADE)
		return 0;
	*dir_x = -gx / len;
	*dir_y = -gy / len;
	if(grade)
		*grade = len / (2.0f * SLIDE_PROBE);

	return 1;
}

int world_spans_free_point(float x, float y, float z) {
	world_span spans[WORLD_SPANS_MAX_PER_TILE];

	if(!isfinite(z))
		return 0;

	int count = world_spans_at(x, y, spans, WORLD_SPANS_MAX_PER_TILE);

	for(int i = 0; i < count; i++) {
		if(spans[i].floor <= z && z < spans[i].ceil)
			return 1;
	}
	return 0;
}

// Refuse paths needing more samples than this, as the 2D path check does.
#define WALK_MAX_STEPS 256

int world_spans_walk(float x0, float y0, float z0, float x1, float y1,
                     float half, float* out_z) {
	if(!isfinite(x0) || !isfinite(y0) || !isfinite(z0))
		return 0;
	if(!isfinite(x1) || !isfinite(y1) || !isfinite(half) || !(g_tile > 0.0f))
		return 0;

	float dx = x1 - x0;
	float dy = y1 - y0;
	float distance = sqrtf(dx * dx + dy * dy);
	float n = distance / (g_tile * 0.5f);
	float z = z0;

	if(!isfinite(n) || n > (float)WALK_MAX_STEPS)
		return 0;

	int steps = (int)n + 1;

	for(int i = 1; i <= steps; i++) {
		float t = (float)i / (float)steps;

		if(!world_spans_reach_box(x0 + dx * t, y0 + dy * t, half, z, &z))
			return 0;
	}
	if(out_z)
		*out_z = z;
	return 1;
}

int world_spans_snap(float x, float y, float hint_z, float* out_z) {
	world_span spans[WORLD_SPANS_MAX_PER_TILE];
	int best = -1;
	float best_gap = 0.0f;

	if(!isfinite(hint_z))
		return 0;

	int count = world_spans_at(x, y, spans, WORLD_SPANS_MAX_PER_TILE);

	for(int i = 0; i < count; i++) {
		float gap = fabsf(spans[i].floor - hint_z);

		if(!span_usable(&spans[i]))
			continue;
		if(best < 0 || gap < best_gap) {
			best = i;
			best_gap = gap;
		}
	}
	if(best < 0)
		return 0;
	if(out_z)
		*out_z = spans[best].floor;
	return 1;
}

static int read_header(FILE* f, spans_header* h) {
	char magic[WORLD_SPANS_MAGIC_LEN];
	uint32_t version = 0;

	if(fread(magic, 1, sizeof(magic), f) != sizeof(magic))
		return 0;
	if(memcmp(magic, WORLD_SPANS_MAGIC, sizeof(magic)) != 0)
		return 0;
	if(fread(&version, 4, 1, f) != 1 || version != WORLD_SPANS_VERSION)
		return 0;
	if(fread(&h->width, 4, 1, f) != 1 || fread(&h->height, 4, 1, f) != 1)
		return 0;
	if(fread(&h->tile_size, 4, 1, f) != 1)
		return 0;
	return fread(&h->tile_count, 4, 1, f) == 1 && fread(&h->span_count, 4, 1, f) == 1;
}

static int header_fits(const spans_header* h) {
	if(h->width != g_width || h->height != g_height)
		return 0;
	if((float)h->tile_size != g_tile)
		return 0;
	return h->tile_count <= WORLD_SPANS_MAX_TILES &&
	       h->span_count <= WORLD_SPANS_MAX_SPANS;
}

static int span_valid(const world_span* s) {
	return isfinite(s->floor) && isfinite(s->ceil) && s->ceil > s->floor;
}

// Records sorted and contiguous, spans finite, ascending and not overlapping.
static int tables_valid(const tile_record* tiles, uint32_t tile_count,
                        const world_span* spans, uint32_t span_count) {
	uint64_t cells = (uint64_t)g_width * (uint64_t)g_height;
	uint32_t next_span = 0;

	for(uint32_t i = 0; i < tile_count; i++) {
		const tile_record* t = &tiles[i];

		if(t->tile_index >= cells)
			return 0;
		if(i > 0 && t->tile_index <= tiles[i - 1].tile_index)
			return 0;
		if(t->span_count > WORLD_SPANS_MAX_PER_TILE || t->first_span != next_span)
			return 0;
		if(t->span_count > span_count - next_span)
			return 0;

		for(uint32_t k = 0; k < t->span_count; k++) {
			const world_span* s = &spans[t->first_span + k];

			if(!span_valid(s))
				return 0;
			if(k > 0 && s->floor < spans[t->first_span + k - 1].ceil)
				return 0;
		}
		next_span += t->span_count;
	}
	return next_span == span_count;
}

// Bytes the header says the file holds, or 0 when that overflows.
static long expected_size(const spans_header* h) {
	return (long)WORLD_SPANS_HEADER_BYTES +
	       (long)h->tile_count * WORLD_SPANS_TILE_BYTES +
	       (long)h->span_count * WORLD_SPANS_SPAN_BYTES;
}

static int file_size_is(FILE* f, long want) {
	long here = ftell(f);

	if(here < 0 || fseek(f, 0, SEEK_END) != 0)
		return 0;

	long size = ftell(f);

	return size == want && fseek(f, here, SEEK_SET) == 0;
}

// Read world_footing.bin from the directory of the spans file, if it is there
static void load_sibling_mask(const char* spans_path) {
	char path[512];
	const char* slash = strrchr(spans_path, '/');
	const char* back = strrchr(spans_path, '\\');
	int dir_len;

	if(back && (!slash || back > slash))
		slash = back;
	dir_len = slash ? (int)(slash - spans_path) + 1 : 0;
	if(snprintf(path, sizeof(path), "%.*sworld_footing.bin", dir_len, spans_path) >=
	   (int)sizeof(path)) {
		free_mask();
		return;
	}
	world_spans_load_mask(path);
}

int world_spans_load(const char* path) {
	spans_header h;
	FILE* f = fopen(path, "rb");

	if(!f)
		return 0;
	if(g_width <= 0 || !read_header(f, &h) || !header_fits(&h)) {
		fclose(f);
		return 0;
	}
	if(!file_size_is(f, expected_size(&h))) {
		fclose(f);
		return 0;
	}

	// the loaded flag is "has tiles", so a file with none still needs one
	tile_record* tiles = calloc(h.tile_count ? h.tile_count : 1, sizeof(*tiles));
	world_span* spans = calloc(h.span_count ? h.span_count : 1, sizeof(*spans));
	int ok = tiles && spans &&
	         fread(tiles, sizeof(*tiles), h.tile_count, f) == h.tile_count &&
	         fread(spans, sizeof(*spans), h.span_count, f) == h.span_count &&
	         tables_valid(tiles, h.tile_count, spans, h.span_count);

	fclose(f);
	if(!ok) {
		free(tiles);
		free(spans);
		return 0;
	}

	free_tables();
	g_tiles = tiles;
	g_spans = spans;
	g_tile_count = h.tile_count;
	g_span_count = h.span_count;
	load_sibling_mask(path);
	return 1;
}
