#ifndef FOOTING_FIXTURE_H
#define FOOTING_FIXTURE_H

// Test footing: flat tile, stairs (4 cells, +4 each), half-unit ramp, 0 and 60

#include "world_regions.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define FF_CELLS 16
#define FF_VERSION 4u

_Static_assert(WORLD_TILE_PX == FF_CELLS, "the fixture's cells are one unit");

// World x of the middle of cell `cell` of fixture tile `tile`
#define FF_X(tile, cell) \
	(((float)(CITY_ENNARA_TILE_X + (tile))) * (float)WORLD_TILE_PX + (float)(cell) + 0.5f)
// World y of the middle of the fixture's row
#define FF_Y ((float)CITY_ENNARA_TILE_Y * (float)WORLD_TILE_PX + 8.0f)

static inline void ff_put_u32(FILE* f, uint32_t v) {
	uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16),
		             (uint8_t)(v >> 24) };

	fwrite(b, 1, 4, f);
}

static inline void ff_put_f32(FILE* f, float v) {
	uint32_t bits;

	memcpy(&bits, &v, sizeof(bits));
	ff_put_u32(f, bits);
}

static inline int ff_stairs(int i, int j) {
	(void)j;

	return i / 4 * 16;
}

static inline int ff_ramp(int i, int j) {
	(void)j;

	return i * 2;
}

static inline int ff_flat(int i, int j) {
	(void)i;
	(void)j;

	return 0;
}

static inline void ff_level(FILE* f, float base, int (*delta)(int, int)) {
	ff_put_f32(f, base);
	for(int j = 0; j < FF_CELLS; j++) {
		for(int i = 0; i < FF_CELLS; i++)
			fputc(delta(i, j), f);
	}
	for(int k = 0; k < FF_CELLS * FF_CELLS; k++)
		fputc(255, f);                // open sky above every floor
}

// `version` and `min_x` let a test write a file the loader must refuse
static inline void ff_write_at(const char* path, uint32_t version, int32_t min_x) {
	FILE* f = fopen(path, "wb");

	fwrite("CFTG", 1, 4, f);
	ff_put_u32(f, version);
	ff_put_u32(f, (uint32_t)min_x);
	ff_put_u32(f, 0);             // min_y
	ff_put_u32(f, 4);             // width
	ff_put_u32(f, 1);             // height
	ff_put_u32(f, FF_CELLS);
	ff_put_u32(f, 4);             // levels in all
	fputc(0, f);                  // tile 0: flat
	fputc(1, f);                  // tile 1: stairs
	ff_level(f, 0.0f, ff_stairs);
	fputc(1, f);                  // tile 2: ramp
	ff_level(f, 0.0f, ff_ramp);
	fputc(2, f);                  // tile 3: two levels
	ff_level(f, 0.0f, ff_flat);
	ff_level(f, 60.0f, ff_flat);
	fclose(f);
}

static inline void ff_write(const char* path, uint32_t version) {
	ff_write_at(path, version, 0);
}

// Copy the first `bytes` bytes of a file, for a truncated copy
static inline void ff_prefix(const char* from, const char* to, long bytes) {
	uint8_t buffer[4096];
	FILE* in = fopen(from, "rb");
	FILE* out = fopen(to, "wb");
	size_t got = fread(buffer, 1, sizeof(buffer) < (size_t)bytes ? sizeof(buffer) : (size_t)bytes, in);

	fwrite(buffer, 1, got, out);
	fclose(in);
	fclose(out);
}

#endif /* FOOTING_FIXTURE_H */
