// Check the seats module shared by client and server: any count, bad files.

#include "world_seats.h"
#include "world_regions.h"

#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define TILE ((float)WORLD_TILE_PX)
#define SCRATCH "/tmp/world_seats_test.bin"

typedef struct {
	const char* name;
	float x, y, z, heading;
} marker;

// Writes a seats file as the exporter does; the header can lie, trim cuts it.
static void write_seats(const marker* m, int n, const char* magic,
		int32_t version, int32_t declared, size_t trim) {
	FILE* f = fopen(SCRATCH, "wb");

	assert(f);
	fwrite(magic, 1, 4, f);
	fwrite(&version, 4, 1, f);
	fwrite(&declared, 4, 1, f);
	for(int i = 0; i < n; i++) {
		char name[32] = { 0 };
		float v[4] = { m[i].x, m[i].y, m[i].z, m[i].heading };

		strncpy(name, m[i].name, sizeof(name) - 1);
		fwrite(v, 4, 4, f);
		fwrite(name, 1, sizeof(name), f);
	}
	fclose(f);

	if(trim) {
		long size;

		f = fopen(SCRATCH, "rb");
		fseek(f, 0, SEEK_END);
		size = ftell(f);
		fclose(f);
		assert(truncate(SCRATCH, size - (long)trim) == 0);
	}
}

static void test_any_count(void) {
	const marker three[] = {
		{ "SIT_Bench_A", 2.5f, 8.5f, 10.0f, 3.14159265f },
		{ "SIT_Bench_B", -12.5f, 2.5f, 10.0f, 1.5707963f },
		{ "SIT_Bench_C", 8.5f, 8.5f, 10.0f, -1.5707963f },
	};
	const world_seat* b;

	write_seats(three, 3, "CSIT", 1, 3, 0);
	assert(world_seats_load(SCRATCH) == 1);
	assert(world_seats_count() == 3);

	// positions come back in world units, from the courtyard-centre tile
	b = world_seats_get(1);
	assert(b && strcmp(b->name, "SIT_Bench_B") == 0);
	assert(fabsf(b->x - (CITY_ENNARA_TILE_X - 12.5f) * TILE) < 0.01f);
	assert(fabsf(b->y - (CITY_ENNARA_TILE_Y + 2.5f) * TILE) < 0.01f);
	assert(b->z == 10.0f && fabsf(b->heading - 1.5707963f) < 1e-5f);
	assert(world_seats_get(-1) == NULL && world_seats_get(3) == NULL);

	// one fewer in the blend is simply one fewer here
	write_seats(three, 2, "CSIT", 1, 2, 0);
	assert(world_seats_load(SCRATCH) == 1);
	assert(world_seats_count() == 2);
	assert(world_seats_get(2) == NULL);

	// none is a valid file: nothing to sit on, nothing wrong
	write_seats(three, 0, "CSIT", 1, 0, 0);
	assert(world_seats_load(SCRATCH) == 1);
	assert(world_seats_count() == 0);
	assert(world_seats_nearest(0.0f, 0.0f, 1.0e9f) == -1);
}

static void test_reach_constant(void) {
	// about 3 m of a 1.83 m character that is 40 units tall
	assert(WORLD_SEAT_REACH_UNITS > 60.0f && WORLD_SEAT_REACH_UNITS < 70.0f);
}

static void test_nearest(void) {
	const marker two[] = {
		{ "SIT_A", 0.0f, 0.0f, 10.0f, 0.0f },
		{ "SIT_B", 10.0f, 0.0f, 10.0f, 0.0f },
	};
	float ox = CITY_ENNARA_TILE_X * TILE;
	float oy = CITY_ENNARA_TILE_Y * TILE;

	write_seats(two, 2, "CSIT", 1, 2, 0);
	assert(world_seats_load(SCRATCH) == 1);

	assert(world_seats_nearest(ox + 1.0f, oy, 40.0f) == 0);
	assert(world_seats_nearest(ox + 9.0f * TILE, oy, 40.0f) == 1);
	assert(world_seats_nearest(ox + 4.0f * TILE, oy + 1.0f, 1.0e6f) == 0);
	assert(world_seats_nearest(ox + 6.0f * TILE, oy + 1.0f, 1.0e6f) == 1);
	assert(world_seats_nearest(ox + 5.0f * TILE, oy + 500.0f, 40.0f) == -1);
}

static void test_keeper_seats(void) {
	const marker mixed[] = {
		{ "SIT_Bench_A", 0.0f, 0.0f, 10.0f, 0.0f },
		{ "SIT_Keeper_A", 10.0f, 0.0f, 10.0f, 0.0f },
		{ "SIT_Keeper_B", 20.0f, 0.0f, 10.0f, 0.0f },
	};
	float ox = CITY_ENNARA_TILE_X * TILE;
	float oy = CITY_ENNARA_TILE_Y * TILE;

	write_seats(mixed, 3, "CSIT", 1, 3, 0);
	assert(world_seats_load(SCRATCH) == 1);

	// only the keeper-named seats are reserved
	assert(!world_seat_is_reserved(world_seats_get(0)));
	assert(world_seat_is_reserved(world_seats_get(1)));
	assert(world_seat_is_reserved(world_seats_get(2)));
	assert(!world_seat_is_reserved(NULL));

	// a keeper lookup ignores the bench standing nearer
	assert(world_seats_keeper_near(ox + 1.0f, oy, 1.0e6f) == 1);
	assert(world_seats_keeper_near(ox + 10.0f * TILE + 3.0f, oy, 8.0f) == 1);
	assert(world_seats_keeper_near(ox + 20.0f * TILE, oy + 2.0f, 8.0f) == 2);
	assert(world_seats_keeper_near(ox + 15.0f * TILE, oy + 500.0f, 40.0f) == -1);
	assert(world_seats_keeper_near(ox, oy, 8.0f) == -1);

	// no seats loaded, no keeper
	world_seats_free();
	assert(world_seats_keeper_near(0.0f, 0.0f, 1.0e9f) == -1);
}

static void test_bad_files_are_refused(void) {
	const marker one[] = { { "SIT_A", 1.0f, 1.0f, 10.0f, 0.0f } };
	const marker nan[] = { { "SIT_A", NAN, 1.0f, 10.0f, 0.0f } };

	assert(world_seats_load("/tmp/world_seats_does_not_exist.bin") == 0);
	assert(world_seats_count() == 0);

	write_seats(one, 1, "XXXX", 1, 1, 0);
	assert(world_seats_load(SCRATCH) == 0);
	write_seats(one, 1, "CSIT", 2, 1, 0);
	assert(world_seats_load(SCRATCH) == 0);

	// says two, holds one
	write_seats(one, 1, "CSIT", 1, 2, 0);
	assert(world_seats_load(SCRATCH) == 0);

	// cut short, and a negative count
	write_seats(one, 1, "CSIT", 1, 1, 7);
	assert(world_seats_load(SCRATCH) == 0);
	write_seats(one, 1, "CSIT", 1, -1, 0);
	assert(world_seats_load(SCRATCH) == 0);
	write_seats(nan, 1, "CSIT", 1, 1, 0);
	assert(world_seats_load(SCRATCH) == 0);

	// a refused file leaves nothing loaded
	assert(world_seats_count() == 0);
}

#ifdef WORLD_SEATS_BAKED_PATH
// The file the exporter writes: whatever it holds must be usable.
static void test_baked_file(void) {
	int n;
	int keepers = 0;
	float ox = CITY_ENNARA_TILE_X * TILE;
	float oy = CITY_ENNARA_TILE_Y * TILE;

	assert(world_seats_load(WORLD_SEATS_BAKED_PATH) == 1);
	n = world_seats_count();
	for(int i = 0; i < n; i++) {
		const world_seat* s = world_seats_get(i);

		assert(s && strncmp(s->name, "SIT_", 4) == 0);
		assert(fabsf(s->x - ox) < 100.0f * TILE && fabsf(s->y - oy) < 100.0f * TILE);
		assert(s->z > 0.0f && fabsf(s->heading) <= 3.15f);
		assert(world_seats_nearest(s->x, s->y, 1.0f) >= 0);
	}
	// each shopkeeper sits at a baked keeper seat, reserved from players
	for(int i = 0; i < n; i++)
		keepers += world_seat_is_reserved(world_seats_get(i));
	printf("world_seats_test: baked file holds %d seats, %d reserved\n", n, keepers);
}
#endif

int main(void) {
	assert(world_seats_count() == 0);
	test_any_count();
	test_reach_constant();
	test_nearest();
	test_keeper_seats();
	test_bad_files_are_refused();
#ifdef WORLD_SEATS_BAKED_PATH
	test_baked_file();
#endif
	world_seats_free();
	assert(world_seats_count() == 0);
	printf("world_seats_test: OK\n");

	return 0;
}
