#ifndef WORLD_MASK_FIXTURE_H
#define WORLD_MASK_FIXTURE_H

// Writes small floor masks (world_footing.bin, format v4) for tests

#include "world_spans.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

// One tile's mask: level k has floor[k]; cols[k] bit ci = floor in column ci,
// delta[k][ci] quarter units above floor[k]; clear[k][ci] is the room above in
// 2-unit steps, 0 meaning open sky and 254 a steep floor (open above)
typedef struct {
	int tx;
	int ty;
	int levels;
	uint16_t cols[WORLD_SPANS_MAX_PER_TILE];
	float floor[WORLD_SPANS_MAX_PER_TILE];
	uint8_t clear[WORLD_SPANS_MAX_PER_TILE][16];
	uint8_t delta[WORLD_SPANS_MAX_PER_TILE][16];
} mask_rec;

static inline void mask_u32(FILE* f, uint32_t v) {
	uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16),
		             (uint8_t)(v >> 24) };

	fwrite(b, 1, 4, f);
}

static inline void mask_f32(FILE* f, float v) {
	uint32_t bits;

	memcpy(&bits, &v, sizeof(bits));
	mask_u32(f, bits);
}

// A width x height grid from (min_x, min_y); tiles without a record get none
static inline void mask_write(const char* path, uint32_t version, int min_x, int min_y,
                              int width, int height, const mask_rec* recs, int nrec) {
	FILE* f = fopen(path, "wb");
	uint32_t levels = 0;

	for(int i = 0; i < nrec; i++)
		levels += (uint32_t)recs[i].levels;
	fwrite("CFTG", 1, 4, f);
	mask_u32(f, version);
	mask_u32(f, (uint32_t)min_x);
	mask_u32(f, (uint32_t)min_y);
	mask_u32(f, (uint32_t)width);
	mask_u32(f, (uint32_t)height);
	mask_u32(f, 16u);
	mask_u32(f, levels);
	for(int ty = min_y; ty < min_y + height; ty++) {
		for(int tx = min_x; tx < min_x + width; tx++) {
			const mask_rec* r = NULL;

			for(int i = 0; i < nrec; i++)
				if(recs[i].tx == tx && recs[i].ty == ty)
					r = &recs[i];
			fputc(r ? r->levels : 0, f);
			for(int k = 0; r && k < r->levels; k++) {
				mask_f32(f, r->floor[k]);
				for(int j = 0; j < 16; j++)
					for(int i = 0; i < 16; i++)
						fputc((r->cols[k] >> i) & 1 ? r->delta[k][i] : 255, f);
				for(int j = 0; j < 16; j++)
					for(int i = 0; i < 16; i++)
						fputc(r->clear[k][i] ? r->clear[k][i] : 255, f);
			}
		}
	}
	fclose(f);
}

// Copy the first `bytes` bytes of a file, for a truncated copy
static inline void mask_prefix(const char* from, const char* to, long bytes) {
	uint8_t buffer[4096];
	FILE* in = fopen(from, "rb");
	FILE* out = fopen(to, "wb");
	size_t want = (size_t)bytes < sizeof(buffer) ? (size_t)bytes : sizeof(buffer);
	size_t got = fread(buffer, 1, want, in);

	fwrite(buffer, 1, got, out);
	fclose(in);
	fclose(out);
}

#endif /* WORLD_MASK_FIXTURE_H */
