/**
 * @file
 * Read the city's baked walkable spans and write the world_spans.bin sidecar.
 *
 * Game/tools/export_city_spans.py writes the baked file from the COLLISION
 * collection of the city .blend. Layout, little-endian:
 *
 *   char   magic[4]   "CSPN"
 *   int32  version
 *   int32  min_x      first column, in tiles from the courtyard centre
 *   int32  min_y      first row, in tiles from the courtyard centre
 *   int32  width      columns
 *   int32  height     rows
 *   per tile, row-major:
 *       uint8 count
 *       count times: float32 floor, float32 ceil (world units)
 *
 * The sidecar layout is world_spans_format.h.
 */
#include "world/worldgen.h"
#include "world_regions.h"
#include "world_spans_format.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FILE_MAGIC   "CSPN"
#define FILE_VERSION 1

/** Refuse a grid no city could have rather than allocating it. */
#define MAX_GRID_SIDE 20000

typedef struct {
	float floor;
	float ceil;
} baked_span;

typedef struct {
	uint32_t first;
	uint32_t count;
} baked_tile;

static baked_tile* g_tiles;
static baked_span* g_spans;
static uint32_t    g_span_total;
static int         g_width;
static int         g_height;
static int         g_min_x;
static int         g_min_y;

static int read_i32(FILE* f, int32_t* out) {
	uint8_t b[4];

	if(fread(b, 1, 4, f) != 4)
		return 0;
	*out = (int32_t)((uint32_t)b[0] | ((uint32_t)b[1] << 8) |
	                 ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24));
	return 1;
}

static int read_f32(FILE* f, float* out) {
	int32_t bits = 0;

	if(!read_i32(f, &bits))
		return 0;
	memcpy(out, &bits, sizeof(*out));
	return 1;
}

/** Read every tile's spans; 0 on a short file or a malformed span. */
static int read_tiles(FILE* f) {
	size_t cells = (size_t)g_width * (size_t)g_height;
	size_t cap = 1024;

	g_spans = malloc(cap * sizeof(baked_span));
	if(!g_spans)
		return 0;
	for(size_t i = 0; i < cells; i++) {
		int c = fgetc(f);

		if(c == EOF || c > WORLD_SPANS_MAX_PER_TILE)
			return 0;
		g_tiles[i].first = g_span_total;
		g_tiles[i].count = (uint32_t)c;
		for(int k = 0; k < c; k++) {
			baked_span s;

			if(!read_f32(f, &s.floor) || !read_f32(f, &s.ceil))
				return 0;
			if(!(s.ceil > s.floor))
				return 0;
			// Ascending and not overlapping, as world_spans_load demands.
			if(k > 0 && s.floor < g_spans[g_span_total - 1].ceil)
				return 0;
			if(g_span_total == cap) {
				baked_span* grown = realloc(g_spans, cap * 2 * sizeof(baked_span));

				if(!grown)
					return 0;
				g_spans = grown;
				cap *= 2;
			}
			g_spans[g_span_total++] = s;
		}
	}
	return 1;
}

void worldgen_city_spans_free(void) {
	free(g_tiles);
	free(g_spans);
	g_tiles = NULL;
	g_spans = NULL;
	g_span_total = 0;
	g_width = 0;
	g_height = 0;
}

int worldgen_city_spans_load(const char* path) {
	worldgen_city_spans_free();

	FILE* f = fopen(path, "rb");

	if(!f) {
		fprintf(stderr, "[WORLDGEN] Cannot open %s\n", path);
		return 0;
	}

	char magic[4];
	int32_t version = 0, min_x = 0, min_y = 0, width = 0, height = 0;
	int ok = fread(magic, 1, 4, f) == 4 && memcmp(magic, FILE_MAGIC, 4) == 0 &&
	         read_i32(f, &version) && version == FILE_VERSION &&
	         read_i32(f, &min_x) && read_i32(f, &min_y) &&
	         read_i32(f, &width) && read_i32(f, &height) &&
	         width > 0 && height > 0 &&
	         width <= MAX_GRID_SIDE && height <= MAX_GRID_SIDE;

	if(ok) {
		g_width = width;
		g_height = height;
		g_min_x = min_x;
		g_min_y = min_y;
		g_tiles = calloc((size_t)width * (size_t)height, sizeof(baked_tile));
		ok = g_tiles != NULL && read_tiles(f);
	}
	fclose(f);

	if(!ok) {
		fprintf(stderr, "[WORLDGEN] %s is not a valid city spans file\n", path);
		worldgen_city_spans_free();
	}
	return ok;
}

/** The baked tile record of world tile (x, y), or NULL outside the grid. */
static const baked_tile* tile_at(int x, int y) {
	if(!g_tiles)
		return NULL;

	int col = x - CITY_ENNARA_TILE_X - g_min_x;
	int row = y - CITY_ENNARA_TILE_Y - g_min_y;

	if(col < 0 || col >= g_width || row < 0 || row >= g_height)
		return NULL;
	return &g_tiles[(size_t)row * g_width + col];
}

int worldgen_city_spans_count(int x, int y) {
	const baked_tile* t = tile_at(x, y);

	return t ? (int)t->count : -1;
}

/** A tile that is one span from 0 up with no ceiling is the default. */
static int is_flat_default(const baked_tile* t) {
	return t->count == 1 && g_spans[t->first].floor == 0.0f &&
	       g_spans[t->first].ceil >= WORLD_SPAN_OPEN_CEIL;
}

static int write_u32(FILE* f, uint32_t v) {
	return fwrite(&v, sizeof(v), 1, f) == 1;
}

static int write_i32(FILE* f, int32_t v) {
	return fwrite(&v, sizeof(v), 1, f) == 1;
}

/** The world tile of baked (col, row), and whether it gets a record: it lies
 *  inside the world and is not the flat default. */
static int wants_record(int col, int row, int world_w, int world_h, int* wx,
                        int* wy) {
	*wx = CITY_ENNARA_TILE_X + g_min_x + col;
	*wy = CITY_ENNARA_TILE_Y + g_min_y + row;
	if(*wx < 0 || *wx >= world_w || *wy < 0 || *wy >= world_h)
		return 0;
	return !is_flat_default(&g_tiles[(size_t)row * g_width + col]);
}

/** Count the tiles and spans the sidecar will carry. */
static void count_records(int world_w, int world_h, uint32_t* tiles,
                          uint32_t* spans) {
	int wx, wy;

	*tiles = 0;
	*spans = 0;
	for(int row = 0; row < g_height; row++) {
		for(int col = 0; col < g_width; col++) {
			if(!wants_record(col, row, world_w, world_h, &wx, &wy))
				continue;
			(*tiles)++;
			*spans += g_tiles[(size_t)row * g_width + col].count;
		}
	}
}

int worldgen_city_spans_write(const char* path, int world_w, int world_h,
                              int tile_size) {
	if(!g_tiles || !path || world_w <= 0 || world_h <= 0 || tile_size <= 0)
		return 0;

	uint32_t tiles, spans;

	count_records(world_w, world_h, &tiles, &spans);

	FILE* f = fopen(path, "wb");

	if(!f)
		return 0;

	int ok = fwrite(WORLD_SPANS_MAGIC, 1, WORLD_SPANS_MAGIC_LEN, f) ==
	             WORLD_SPANS_MAGIC_LEN &&
	         write_u32(f, WORLD_SPANS_VERSION) && write_i32(f, world_w) &&
	         write_i32(f, world_h) && write_i32(f, tile_size) &&
	         write_u32(f, tiles) && write_u32(f, spans);
	uint32_t next_first = 0;
	int wx, wy;

	// Records first, in tile-index order: rows ascend, then columns.
	for(int row = 0; ok && row < g_height; row++) {
		for(int col = 0; ok && col < g_width; col++) {
			if(!wants_record(col, row, world_w, world_h, &wx, &wy))
				continue;

			uint32_t count = g_tiles[(size_t)row * g_width + col].count;

			ok = write_u32(f, (uint32_t)wy * (uint32_t)world_w + (uint32_t)wx) &&
			     write_u32(f, next_first) && write_u32(f, count);
			next_first += count;
		}
	}
	for(int row = 0; ok && row < g_height; row++) {
		for(int col = 0; ok && col < g_width; col++) {
			if(!wants_record(col, row, world_w, world_h, &wx, &wy))
				continue;

			const baked_tile* t = &g_tiles[(size_t)row * g_width + col];

			ok = fwrite(&g_spans[t->first], sizeof(baked_span), t->count, f) == t->count;
		}
	}
	if(fclose(f) != 0)
		ok = 0;
	if(!ok)
		remove(path);
	return ok;
}
