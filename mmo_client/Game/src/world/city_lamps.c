// The lamps that light the city at night.

#include "world/city_lamps.h"
#include "model/json.h"

#include <stdio.h>
#include <stdlib.h>

#define LAMP_FILE_MAX 65536
#define LAMP_DEFAULT_RADIUS_M 10.0f

static const float LAMP_WARM[3] = { 1.00f, 0.72f, 0.38f };

static city_lamp g_lamps[CITY_LAMPS_MAX];
static int g_count;

// The whole file as text, or NULL. The caller frees it.
static char* read_file(const char* path) {
	FILE* f = fopen(path, "rb");
	char* text;
	size_t got;

	if(!f)
		return NULL;
	text = malloc(LAMP_FILE_MAX + 1);
	if(!text) {
		fclose(f);
		return NULL;
	}
	got = fread(text, 1, LAMP_FILE_MAX, f);
	text[got] = '\0';
	fclose(f);

	return text;
}

// A number member of a lamp, or the fallback when it is absent
static float member_float(JsonSlice object, const char* key, float fallback) {
	JsonSlice value;
	float out;

	if(json_member(object, key, &value) && json_float(value, &out))
		return out;

	return fallback;
}

static int read_lamp(JsonSlice object, float ox, float oy, float scale, city_lamp* out) {
	JsonSlice probe;
	float intensity;

	if(!json_member(object, "x", &probe) || !json_member(object, "y", &probe) ||
	   !json_member(object, "z", &probe))
		return 0;
	out->pos[0] = ox + member_float(object, "x", 0.0f) * scale;
	out->pos[1] = member_float(object, "y", 0.0f) * scale;
	out->pos[2] = oy + member_float(object, "z", 0.0f) * scale;
	out->radius = member_float(object, "radius", LAMP_DEFAULT_RADIUS_M) * scale;
	intensity = member_float(object, "intensity", 1.0f);
	for(int i = 0; i < 3; i++)
		out->color[i] = LAMP_WARM[i] * intensity;

	return out->radius > 0.0f;
}

int city_lamps_load(const char* path, float origin_x, float origin_y, float units_per_metre) {
	char* text = read_file(path);
	JsonSlice root, array;
	JsonSlice items[CITY_LAMPS_MAX];
	int count;

	city_lamps_clear();
	if(!text)
		return -1;
	root.begin = text;
	root.length = 0;
	while(text[root.length])
		root.length++;
	if(!json_member(root, "lamps", &array)) {
		free(text);
		return -1;
	}
	count = json_array_index(array, items, CITY_LAMPS_MAX);
	if(count < 0) {
		free(text);
		return -1;
	}
	for(int i = 0; i < count; i++)
		if(!read_lamp(items[i], origin_x, origin_y, units_per_metre, &g_lamps[i])) {
			city_lamps_clear();
			free(text);
			return -1;
		}
	g_count = count;
	free(text);

	return count;
}

void city_lamps_clear(void) {
	g_count = 0;
}

int city_lamps_count(void) {
	return g_count;
}

static float ground_distance_sq(const city_lamp* lamp, float x, float y) {
	float dx = lamp->pos[0] - x;
	float dy = lamp->pos[2] - y;

	return dx * dx + dy * dy;
}

int city_lamps_nearest(float x, float y, int max, city_lamp* out) {
	int taken[CITY_LAMPS_MAX] = { 0 };
	int written = 0;

	while(written < max && written < g_count) {
		int best = -1;

		for(int i = 0; i < g_count; i++)
			if(!taken[i] && (best < 0 ||
			   ground_distance_sq(&g_lamps[i], x, y) < ground_distance_sq(&g_lamps[best], x, y)))
				best = i;
		taken[best] = 1;
		out[written++] = g_lamps[best];
	}

	return written;
}
