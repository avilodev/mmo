// Check the baked spans become the world_spans.bin sidecar, and that the flat
// layer worldgen derives from them agrees with what the span queries find.

#define _POSIX_C_SOURCE 200809L
#include "world/worldgen.h"
#include "world_regions.h"
#include "world_spans.h"
#include "world_spans_format.h"

#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define BAKED_PATH "/tmp/worldgen_city_spans_test_baked.bin"
#define SIDECAR_PATH "/tmp/worldgen_city_spans_test_sidecar.bin"
#define CITY_FOOTING_PATH "Game/assets/baked/test_town_footing.bin"
#define TILE 16
#define WORLD_W (CITY_ENNARA_TILE_X + 100)
#define WORLD_H (CITY_ENNARA_TILE_Y + 100)
#define OPEN WORLD_SPAN_OPEN_CEIL

static void put_i32(FILE* f, int32_t v) {
	fwrite(&v, sizeof(v), 1, f);
}

static void put_tile(FILE* f, int count, const float* pairs) {
	fputc(count, f);
	for(int i = 0; i < count * 2; i++)
		fwrite(&pairs[i], sizeof(float), 1, f);
}

// A 4x2 grid: flat default, a ramp, a tile with no floor, a bridge; then a
// ramp top, a platform, and two default tiles.
static void write_baked(void) {
	FILE* f = fopen(BAKED_PATH, "wb");

	assert(f);
	fwrite("CSPN", 1, 4, f);
	put_i32(f, 1);
	put_i32(f, 10);
	put_i32(f, 20);
	put_i32(f, 4);
	put_i32(f, 2);
	put_tile(f, 1, (float[]){0.0f, OPEN});
	put_tile(f, 1, (float[]){8.0f, OPEN});
	put_tile(f, 0, NULL);
	put_tile(f, 2, (float[]){0.0f, 70.0f, 74.0f, OPEN});
	put_tile(f, 1, (float[]){16.0f, OPEN});
	put_tile(f, 1, (float[]){100.0f, OPEN});
	put_tile(f, 1, (float[]){0.0f, OPEN});
	put_tile(f, 1, (float[]){0.0f, OPEN});
	fclose(f);
}

static float cx(int baked_col) {
	return ((float)(CITY_ENNARA_TILE_X + 10 + baked_col) + 0.5f) * TILE;
}

static float cy(int baked_row) {
	return ((float)(CITY_ENNARA_TILE_Y + 20 + baked_row) + 0.5f) * TILE;
}

static void test_synthetic(void) {
	world_span spans[WORLD_SPANS_MAX_PER_TILE];
	float z;

	write_baked();
	assert(worldgen_city_spans_load(BAKED_PATH));
	assert(worldgen_city_spans_count(CITY_ENNARA_TILE_X + 10, CITY_ENNARA_TILE_Y + 20) == 1);
	assert(worldgen_city_spans_count(CITY_ENNARA_TILE_X + 12, CITY_ENNARA_TILE_Y + 20) == 0);
	assert(worldgen_city_spans_count(CITY_ENNARA_TILE_X + 13, CITY_ENNARA_TILE_Y + 20) == 2);
	assert(worldgen_city_spans_count(CITY_ENNARA_TILE_X + 9, CITY_ENNARA_TILE_Y + 20) == -1);
	assert(worldgen_city_spans_count(CITY_ENNARA_TILE_X + 10, CITY_ENNARA_TILE_Y + 22) == -1);

	assert(worldgen_city_spans_write(SIDECAR_PATH, WORLD_W, WORLD_H, TILE));
	world_spans_init(WORLD_W, WORLD_H, TILE);
	assert(world_spans_load(SIDECAR_PATH));

	assert(world_spans_snap(cx(1), cy(0), 0.0f, &z) && z == 8.0f);        // the ramp
	assert(world_spans_snap(cx(0), cy(0), 0.0f, &z) && z == 0.0f);        // default
	assert(!world_spans_snap(cx(2), cy(0), 0.0f, &z));                    // no floor
	assert(world_spans_at(cx(2), cy(0), spans, WORLD_SPANS_MAX_PER_TILE) == 0);
	assert(world_spans_at(cx(3), cy(0), spans, WORLD_SPANS_MAX_PER_TILE) == 2);
	assert(spans[1].floor == 74.0f);                                      // the bridge
	assert(world_spans_snap(cx(1), cy(1), 100.0f, &z) && z == 100.0f);    // the platform
	assert(world_spans_snap(cx(0), cy(1), 16.0f, &z) && z == 16.0f);      // ramp top
	assert(world_spans_snap(cx(2), cy(1), 0.0f, &z) && z == 0.0f);       // default

	// a tile nothing baked is flat open ground
	assert(world_spans_at(10.0f, 10.0f, spans, WORLD_SPANS_MAX_PER_TILE) == 1);
	assert(spans[0].floor == 0.0f);
	world_spans_shutdown();
	worldgen_city_spans_free();
}

static void test_tiles_outside_the_world_are_skipped(void) {
	world_span spans[WORLD_SPANS_MAX_PER_TILE];

	write_baked();
	assert(worldgen_city_spans_load(BAKED_PATH));
	assert(worldgen_city_spans_write(SIDECAR_PATH, 5, 5, TILE));
	world_spans_init(5, 5, TILE);
	assert(world_spans_load(SIDECAR_PATH));                               // valid, empty
	assert(world_spans_at(10.0f, 10.0f, spans, WORLD_SPANS_MAX_PER_TILE) == 1);
	assert(!worldgen_city_spans_write(SIDECAR_PATH, 0, 5, TILE));
	world_spans_shutdown();
	worldgen_city_spans_free();
}

static void test_bad_files_are_refused(void) {
	FILE* f = fopen(BAKED_PATH, "wb");

	assert(f);
	fwrite("NOPE", 1, 4, f);
	fclose(f);
	assert(!worldgen_city_spans_load(BAKED_PATH));
	assert(!worldgen_city_spans_load("/tmp/worldgen_city_spans_test_missing.bin"));
	assert(worldgen_city_spans_count(CITY_ENNARA_TILE_X, CITY_ENNARA_TILE_Y) == -1);

	write_baked();
	assert(truncate(BAKED_PATH, 40) == 0);
	assert(!worldgen_city_spans_load(BAKED_PATH));
}

// Does some cell of world tile (x, y) have a floor with room for a body?
static int tile_has_floor(int x, int y) {
	float z;

	for(int j = 0; j < TILE; j++) {
		for(int i = 0; i < TILE; i++) {
			if(world_spans_snap((float)(x * TILE + i) + 0.5f, (float)(y * TILE + j) + 0.5f,
			                    0.0f, &z))
				return 1;
		}
	}

	return 0;
}

// For every tile of the real bake, read with its floor mask: no span there
// exactly when no cell of it has a floor, so the flat layer never blocks a
// floor nor allows a void.
static void test_real_bake_agrees_with_the_queries(void) {
	int blocked = 0;
	int walkable = 0;

	assert(worldgen_city_spans_load(CITY_SPANS_PATH));
	assert(worldgen_city_spans_write(SIDECAR_PATH, WORLD_W, WORLD_H, TILE));
	world_spans_init(WORLD_W, WORLD_H, TILE);
	assert(world_spans_load(SIDECAR_PATH));
	assert(world_spans_load_mask(CITY_FOOTING_PATH));

	for(int y = CITY_ENNARA_TILE_Y - 200; y < CITY_ENNARA_TILE_Y + 200; y++) {
		for(int x = CITY_ENNARA_TILE_X - 200; x < CITY_ENNARA_TILE_X + 200; x++) {
			int count = worldgen_city_spans_count(x, y);

			if(count < 0)
				continue;
			assert((count == 0) == !tile_has_floor(x, y));
			blocked += count == 0;
			walkable += count > 0;
		}
	}
	assert(walkable > 1000);

	// the spawn point (the scene origin) must be standable
	float z;

	assert(world_spans_snap(((float)CITY_ENNARA_TILE_X + 0.5f) * TILE,
	                        ((float)CITY_ENNARA_TILE_Y + 0.5f) * TILE, 0.0f, &z));
	assert(fabsf(z) < 2.0f);
	printf("worldgen_city_spans_test: %d walkable, %d blocked tiles\n", walkable, blocked);
	world_spans_shutdown();
	worldgen_city_spans_free();
}

int main(void) {
	remove(SIDECAR_PATH);
	test_synthetic();
	remove(SIDECAR_PATH);
	test_tiles_outside_the_world_are_skipped();
	test_bad_files_are_refused();
	test_real_bake_agrees_with_the_queries();
	remove(SIDECAR_PATH);
	remove(BAKED_PATH);
	puts("worldgen_city_spans_test: all tests passed");
	return 0;
}
