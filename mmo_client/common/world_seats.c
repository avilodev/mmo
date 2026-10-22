/**
 * Load and look up the baked city seats.
 *
 * Layout, little-endian, written by export_city_seats.py:
 *   char magic[4] "CSIT", int32 version, int32 count, then per seat 48 bytes:
 *   float x, y (tiles from the courtyard centre), float z, float heading,
 *   char name[32] (NUL padded).
 */

#include "world_seats.h"
#include "world_regions.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FILE_MAGIC "CSIT"
#define FILE_VERSION 1
#define MAX_SEATS 4096
#define RECORD_BYTES (4 * 4 + WORLD_SEAT_NAME_MAX)

static world_seat* g_seats;
static int g_count;

static uint32_t le32(const uint8_t* b) {
	return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) |
	       ((uint32_t)b[3] << 24);
}

static float le_float(const uint8_t* b) {
	uint32_t bits = le32(b);
	float value;

	memcpy(&value, &bits, sizeof(value));

	return value;
}

void world_seats_free(void) {
	free(g_seats);
	g_seats = NULL;
	g_count = 0;
}

// Returns 0 when a number is not finite.
static int read_seat(const uint8_t* rec, world_seat* out) {
	float tile_x = le_float(rec);
	float tile_y = le_float(rec + 4);

	out->z = le_float(rec + 8);
	out->heading = le_float(rec + 12);
	if(!isfinite(tile_x) || !isfinite(tile_y) || !isfinite(out->z) ||
	   !isfinite(out->heading))
		return 0;

	out->x = ((float)CITY_ENNARA_TILE_X + tile_x) * (float)WORLD_TILE_PX;
	out->y = ((float)CITY_ENNARA_TILE_Y + tile_y) * (float)WORLD_TILE_PX;
	memcpy(out->name, rec + 16, WORLD_SEAT_NAME_MAX);
	out->name[WORLD_SEAT_NAME_MAX - 1] = '\0';

	return 1;
}

// Returns 0 on any malformation.
static int read_file(FILE* f, world_seat** out, int* out_count) {
	uint8_t head[12];
	int32_t count;
	size_t bytes;
	uint8_t* raw;
	world_seat* seats;
	int ok;

	if(fread(head, 1, sizeof(head), f) != sizeof(head))
		return 0;
	if(memcmp(head, FILE_MAGIC, 4) != 0 || le32(head + 4) != FILE_VERSION)
		return 0;

	count = (int32_t)le32(head + 8);
	if(count < 0 || count > MAX_SEATS)
		return 0;

	bytes = (size_t)count * RECORD_BYTES;
	raw = malloc(bytes ? bytes : 1);
	seats = calloc(count ? (size_t)count : 1, sizeof(*seats));
	ok = raw && seats && fread(raw, 1, bytes, f) == bytes && fgetc(f) == EOF;

	for(int i = 0; ok && i < count; i++)
		ok = read_seat(raw + (size_t)i * RECORD_BYTES, &seats[i]);
	free(raw);
	if(!ok) {
		free(seats);
		return 0;
	}
	*out = seats;
	*out_count = count;

	return 1;
}

int world_seats_load(const char* path) {
	FILE* f;
	int ok;

	world_seats_free();
	f = fopen(path, "rb");
	if(!f)
		return 0;

	ok = read_file(f, &g_seats, &g_count);
	fclose(f);
	if(!ok)
		world_seats_free();

	return ok;
}

int world_seats_count(void) {
	return g_count;
}

const world_seat* world_seats_get(int index) {
	if(index < 0 || index >= g_count)
		return NULL;

	return &g_seats[index];
}

int world_seats_nearest(float world_x, float world_y, float max_dist) {
	int best = -1;
	float best_d2 = max_dist * max_dist;

	for(int i = 0; i < g_count; i++) {
		float dx = g_seats[i].x - world_x;
		float dy = g_seats[i].y - world_y;
		float d2 = dx * dx + dy * dy;

		if(d2 <= best_d2) {
			best = i;
			best_d2 = d2;
		}
	}

	return best;
}

int world_seat_is_reserved(const world_seat* seat) {
	if(!seat)
		return 0;

	return strncmp(seat->name, WORLD_SEAT_KEEPER_PREFIX,
	               strlen(WORLD_SEAT_KEEPER_PREFIX)) == 0;
}

int world_seats_keeper_near(float world_x, float world_y, float max_dist) {
	int best = -1;
	float best_d2 = max_dist * max_dist;

	for(int i = 0; i < g_count; i++) {
		float dx = g_seats[i].x - world_x;
		float dy = g_seats[i].y - world_y;
		float d2 = dx * dx + dy * dy;

		if(world_seat_is_reserved(&g_seats[i]) && d2 <= best_d2) {
			best = i;
			best_d2 = d2;
		}
	}

	return best;
}
