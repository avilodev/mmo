// The weather over the whole map, blended from each region's forecast.

#include "world/weather_field.h"
#include "world_regions.h"

#include <math.h>
#include <string.h>

// How much cloud, rain, storm and snow each condition means
typedef struct {
	float cloud;
	float rain;
	float storm;
	float snow;
} condition_look;

static const condition_look LOOKS[WEATHER_CONDITION_COUNT] = {
	{ 0.04f, 0.00f, 0.00f, 0.00f },
	{ 0.38f, 0.00f, 0.00f, 0.00f },
	{ 0.78f, 0.00f, 0.00f, 0.00f },
	{ 0.88f, 0.75f, 0.00f, 0.00f },
	{ 1.00f, 0.90f, 1.00f, 0.00f },
	{ 0.90f, 0.00f, 0.00f, 0.85f }
};

// Size of the biggest and the smaller patches of cover, in world pixels
#define PATCH_LARGE 2400.0f
#define PATCH_SMALL 900.0f

static float clamp01(float value) {
	if(value < 0.0f)
		return 0.0f;

	return value > 1.0f ? 1.0f : value;
}

static float lerp(float a, float b, float t) {
	return a + (b - a) * t;
}

// How much of a condition's rain or snow a day's level keeps: none given is the old full amount
static float level_scale(int intensity) {
	static const float SCALES[4] = { 1.0f, 0.55f, 0.8f, 1.0f };

	return SCALES[intensity < 0 || intensity > 3 ? 0 : intensity];
}

void weather_field_blend(const WeatherDayData* regions, int region_count, float x, float y,
                         weather_sample* out) {
	float total = 0.0f;

	memset(out, 0, sizeof(*out));
	if(!regions || region_count <= 0)
		return;
	if(region_count > WORLD_CITY_COUNT)
		region_count = WORLD_CITY_COUNT;

	for(int i = 0; i < region_count; i++) {
		float dx = x - (float)WORLD_CITIES[i].tile_x * WORLD_TILE_PX;
		float dy = y - (float)WORLD_CITIES[i].tile_y * WORLD_TILE_PX;
		float d = sqrtf(dx * dx + dy * dy);
		float weight = 1.0f / (fmaxf(d, 1.0f) * fmaxf(d, 1.0f) * fmaxf(d, 1.0f));
		const condition_look* look;

		// A condition this build does not know is taken as a clear sky
		look = regions[i].condition < WEATHER_CONDITION_COUNT ? &LOOKS[regions[i].condition]
		                                                       : &LOOKS[WEATHER_CLEAR];
		out->cloud += look->cloud * weight;
		out->rain += look->rain * level_scale(regions[i].intensity) * weight;
		out->storm += look->storm * weight;
		out->snow += look->snow * level_scale(regions[i].intensity) * weight;
		out->temp_c += ((float)regions[i].temp_hi + (float)regions[i].temp_lo) * 0.5f * weight;
		total += weight;
	}

	out->cloud /= total;
	out->rain /= total;
	out->storm /= total;
	out->snow /= total;
	out->temp_c /= total;
}

// A well-mixed number from a lattice point
static unsigned int hash(int x, int y, unsigned int seed) {
	unsigned int h = (unsigned int)x * 374761393u + (unsigned int)y * 668265263u + seed * 2246822519u;

	h = (h ^ (h >> 13)) * 1274126177u;

	return h ^ (h >> 16);
}

// Value noise at one scale: lattice values, eased between
static float value_noise(float x, float y, float cell, unsigned int seed) {
	float fx = x / cell;
	float fy = y / cell;
	int ix = (int)floorf(fx);
	int iy = (int)floorf(fy);
	float tx = fx - (float)ix;
	float ty = fy - (float)iy;
	float v00 = (float)(hash(ix, iy, seed) & 0xFFFFu) / 65535.0f;
	float v10 = (float)(hash(ix + 1, iy, seed) & 0xFFFFu) / 65535.0f;
	float v01 = (float)(hash(ix, iy + 1, seed) & 0xFFFFu) / 65535.0f;
	float v11 = (float)(hash(ix + 1, iy + 1, seed) & 0xFFFFu) / 65535.0f;

	tx = tx * tx * (3.0f - 2.0f * tx);
	ty = ty * ty * (3.0f - 2.0f * ty);

	return lerp(lerp(v00, v10, tx), lerp(v01, v11, tx), ty);
}

float weather_field_noise(float x, float y, unsigned int seed) {
	return clamp01(value_noise(x, y, PATCH_LARGE, seed) * 0.65f +
	               value_noise(x, y, PATCH_SMALL, seed + 101u) * 0.35f);
}

void weather_field_roughen(weather_sample* sample, float x, float y, unsigned int seed) {
	float patch = 0.55f + 0.9f * weather_field_noise(x, y, seed);

	sample->cloud = clamp01(sample->cloud * patch);
	sample->rain = clamp01(sample->rain * patch);
	sample->storm = clamp01(sample->storm * patch);
	sample->snow = clamp01(sample->snow * patch);
}

// The cloud layer's colour: grey with cover, tinted by what falls from it
static void cloud_color(const weather_sample* s, float rgba[4]) {
	float shade = 1.0f - 0.35f * s->cloud;
	float r = 0.96f * shade;
	float g = 0.97f * shade;
	float b = 1.00f * shade;
	float fade = s->cloud < 0.08f ? s->cloud / 0.08f : 1.0f;

	r = lerp(r, 0.30f, s->rain * 0.8f);
	g = lerp(g, 0.50f, s->rain * 0.8f);
	b = lerp(b, 0.90f, s->rain * 0.8f);
	r = lerp(r, 0.50f, s->storm * 0.8f);
	g = lerp(g, 0.28f, s->storm * 0.8f);
	b = lerp(b, 0.78f, s->storm * 0.8f);
	r = lerp(r, 0.92f, s->snow * 0.9f);
	g = lerp(g, 0.97f, s->snow * 0.9f);
	b = lerp(b, 1.00f, s->snow * 0.9f);

	rgba[0] = clamp01(r);
	rgba[1] = clamp01(g);
	rgba[2] = clamp01(b);
	rgba[3] = clamp01((s->cloud * 0.78f + s->rain * 0.12f) * fade);
}

typedef struct {
	float degrees;
	float r;
	float g;
	float b;
} temp_stop;

static const temp_stop TEMP_STOPS[] = {
	{ -20.0f, 0.55f, 0.35f, 0.85f },
	{ -5.0f, 0.30f, 0.50f, 1.00f },
	{ 5.0f, 0.35f, 0.85f, 0.95f },
	{ 15.0f, 0.55f, 0.90f, 0.45f },
	{ 25.0f, 1.00f, 0.85f, 0.30f },
	{ 35.0f, 1.00f, 0.45f, 0.20f },
	{ 45.0f, 0.80f, 0.15f, 0.15f }
};

#define TEMP_STOP_COUNT ((int)(sizeof(TEMP_STOPS) / sizeof(TEMP_STOPS[0])))

static void temperature_color(float celsius, float rgba[4]) {
	int last = TEMP_STOP_COUNT - 1;
	float t;

	rgba[3] = 0.6f;
	if(celsius <= TEMP_STOPS[0].degrees) {
		rgba[0] = TEMP_STOPS[0].r;
		rgba[1] = TEMP_STOPS[0].g;
		rgba[2] = TEMP_STOPS[0].b;
		return;
	}
	if(celsius >= TEMP_STOPS[last].degrees) {
		rgba[0] = TEMP_STOPS[last].r;
		rgba[1] = TEMP_STOPS[last].g;
		rgba[2] = TEMP_STOPS[last].b;
		return;
	}

	for(int i = 1; i <= last; i++) {
		if(celsius > TEMP_STOPS[i].degrees)
			continue;

		t = (celsius - TEMP_STOPS[i - 1].degrees) / (TEMP_STOPS[i].degrees - TEMP_STOPS[i - 1].degrees);
		rgba[0] = lerp(TEMP_STOPS[i - 1].r, TEMP_STOPS[i].r, t);
		rgba[1] = lerp(TEMP_STOPS[i - 1].g, TEMP_STOPS[i].g, t);
		rgba[2] = lerp(TEMP_STOPS[i - 1].b, TEMP_STOPS[i].b, t);
		return;
	}
}

void weather_field_color(weather_layer layer, const weather_sample* sample, float rgba[4]) {
	if(layer == WEATHER_LAYER_TEMPERATURE)
		temperature_color(sample->temp_c, rgba);
	else
		cloud_color(sample, rgba);
}
