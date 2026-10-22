#ifndef WORLD_SPANS_FIXTURE_H
#define WORLD_SPANS_FIXTURE_H

// Writes small span sidecars for tests, including deliberately bad ones.

#include "world_spans.h"
#include "world_spans_format.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define FIX_W 64
#define FIX_H 64
#define FIX_TILE 16
#define FIX_T(x, y) ((uint32_t)((y) * FIX_W + (x)))
#define FIX_CX(tx) (((float)(tx) + 0.5f) * FIX_TILE)
#define FIX_OPEN WORLD_SPAN_OPEN_CEIL

typedef struct {
	uint32_t index;
	uint32_t first;
	uint32_t count;
} fix_record;

// trim drops that many bytes from the end of the file.
static inline void fix_write(const char* path, int w, int h, int tile,
                             const fix_record* recs, uint32_t nrec,
                             const world_span* spans, uint32_t nspan,
                             uint32_t version, size_t trim) {
	FILE* f = fopen(path, "wb");

	assert(f);
	fwrite(WORLD_SPANS_MAGIC, 1, WORLD_SPANS_MAGIC_LEN, f);
	fwrite(&version, 4, 1, f);
	fwrite(&w, 4, 1, f);
	fwrite(&h, 4, 1, f);
	fwrite(&tile, 4, 1, f);
	fwrite(&nrec, 4, 1, f);
	fwrite(&nspan, 4, 1, f);
	fwrite(recs, sizeof(fix_record), nrec, f);
	fwrite(spans, sizeof(world_span), nspan, f);
	fclose(f);

	if(trim) {
		long size;

		f = fopen(path, "rb");
		fseek(f, 0, SEEK_END);
		size = ftell(f);
		fclose(f);
		assert(truncate(path, size - (long)trim) == 0);
	}
}

// One ramp (tiles 10-13 on row 5 rise 0, 8, 16, 24), a tile at 60, a bridge
// over open ground at tile 20, a low ceiling at 22, no floor at 24, and a
// platform at 100 on tile 29 (next to open ground at tile 30).
static inline void fix_write_city(const char* path) {
	static const fix_record recs[] = {
		{FIX_T(10, 5), 0, 1}, {FIX_T(11, 5), 1, 1}, {FIX_T(12, 5), 2, 1},
		{FIX_T(13, 5), 3, 1}, {FIX_T(14, 5), 4, 1}, {FIX_T(20, 5), 5, 2},
		{FIX_T(22, 5), 7, 1}, {FIX_T(24, 5), 8, 0}, {FIX_T(29, 5), 8, 1},
	};
	static const world_span spans[] = {
		{0, FIX_OPEN}, {8, FIX_OPEN}, {16, FIX_OPEN}, {24, FIX_OPEN},
		{60, FIX_OPEN}, {0, 70}, {74, FIX_OPEN}, {0, 30}, {100, FIX_OPEN},
	};

	fix_write(path, FIX_W, FIX_H, FIX_TILE, recs, 9, spans, 9,
	          WORLD_SPANS_VERSION, 0);
}

#endif /* WORLD_SPANS_FIXTURE_H */
