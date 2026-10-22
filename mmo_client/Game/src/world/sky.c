// The colours and lights of the sky, from the time of day.

#include "world/sky.h"
#include "render/world_light.h"

#include <math.h>

// Where the sun's path leans: negative is toward the north
#define SKY_SUN_TILT_Z -0.4f
#define SKY_KEY_COUNT 10
#define SKY_PI 3.14159265f
// The haze below the horizon is a little darker than the sky above it
#define SKY_GROUND_HAZE 0.85f

typedef struct {
	float hour;
	float zenith[3];
	float horizon[3];
} sky_key;

// Noon keeps the sky colour the world was fogged with before the sky moved
static const sky_key KEYS[SKY_KEY_COUNT] = {
	{ 0.0f, { 0.03f, 0.04f, 0.12f }, { 0.07f, 0.09f, 0.20f } },
	{ 4.5f, { 0.03f, 0.04f, 0.12f }, { 0.07f, 0.09f, 0.20f } },
	{ 6.0f, { 0.32f, 0.42f, 0.72f }, { 0.98f, 0.62f, 0.45f } },
	{ 7.5f, { 0.33f, 0.58f, 0.90f }, { 0.74f, 0.84f, 0.94f } },
	{ 12.0f, { 0.25f, 0.52f, 0.92f }, { WORLD_SKY_R, WORLD_SKY_G, WORLD_SKY_B } },
	{ 16.5f, { 0.30f, 0.54f, 0.90f }, { 0.72f, 0.80f, 0.90f } },
	{ 18.0f, { 0.30f, 0.30f, 0.62f }, { 1.00f, 0.52f, 0.30f } },
	{ 19.5f, { 0.10f, 0.10f, 0.28f }, { 0.38f, 0.22f, 0.38f } },
	{ 21.0f, { 0.03f, 0.04f, 0.12f }, { 0.07f, 0.09f, 0.20f } },
	{ 24.0f, { 0.03f, 0.04f, 0.12f }, { 0.07f, 0.09f, 0.20f } },
};

static const float SUN_LOW[3] = { 1.00f, 0.55f, 0.25f };
static const float SUN_HIGH[3] = { 1.00f, 0.96f, 0.85f };

static sky_state g_current;
static int g_current_set;

static float clamp01(float x) {
	if(x < 0.0f)
		return 0.0f;
	if(x > 1.0f)
		return 1.0f;

	return x;
}

static float smooth_step(float lo, float hi, float x) {
	float t = clamp01((x - lo) / (hi - lo));

	return t * t * (3.0f - 2.0f * t);
}

static void mix3(const float* a, const float* b, float t, float* out) {
	for(int i = 0; i < 3; i++)
		out[i] = a[i] + (b[i] - a[i]) * t;
}

static float wrap_hour(float hour) {
	float h = fmodf(hour, 24.0f);

	if(h < 0.0f)
		h += 24.0f;
	if(h >= 24.0f)
		h = 0.0f;

	return h;
}

static void sun_at(float hour, sky_state* out) {
	float a = hour / 24.0f * 2.0f * SKY_PI - SKY_PI * 0.5f;
	float x = cosf(a);
	float y = sinf(a);
	float z = SKY_SUN_TILT_Z;
	float len = sqrtf(x * x + y * y + z * z);

	out->sun_dir[0] = x / len;
	out->sun_dir[1] = y / len;
	out->sun_dir[2] = z / len;
	for(int i = 0; i < 3; i++)
		out->moon_dir[i] = -out->sun_dir[i];
	mix3(SUN_LOW, SUN_HIGH, smooth_step(0.0f, 0.35f, out->sun_dir[1]), out->sun_color);
	out->star_amount = 1.0f - smooth_step(-0.2f, 0.05f, out->sun_dir[1]);
}

void sky_at(float hour, sky_state* out) {
	float h = wrap_hour(hour);
	int i = 0;
	float t;

	while(i < SKY_KEY_COUNT - 2 && h >= KEYS[i + 1].hour)
		i++;
	t = (h - KEYS[i].hour) / (KEYS[i + 1].hour - KEYS[i].hour);
	mix3(KEYS[i].zenith, KEYS[i + 1].zenith, t, out->zenith);
	mix3(KEYS[i].horizon, KEYS[i + 1].horizon, t, out->horizon);
	for(int k = 0; k < 3; k++)
		out->haze[k] = out->horizon[k] * SKY_GROUND_HAZE;
	sun_at(h, out);
}

// How much of the sky's colour is lost to grey under full overcast
#define SKY_OVERCAST_GREY 0.75f
#define SKY_OVERCAST_DARK 0.35f
#define SKY_STORM_DARK 0.30f

// The colour of lightning on the sky
static const float FLASH_COLOR[3] = { 0.55f, 0.60f, 0.85f };

static float luminance(const float* c) {
	return 0.30f * c[0] + 0.59f * c[1] + 0.11f * c[2];
}

void sky_apply_weather(sky_state* sky, const weather_now* weather) {
	float over = smooth_step(0.2f, 1.0f, weather->cloud);
	// Falling snow scatters the light, so a snow sky stays bright
	float dark = 1.0f - SKY_OVERCAST_DARK * over * (1.0f - 0.6f * weather->snow) -
	             SKY_STORM_DARK * weather->storm * over;
	float* layers[2] = { sky->zenith, sky->horizon };
	// The horizon keeps more of a sunrise's colour than the sky overhead
	float grey[2] = { SKY_OVERCAST_GREY * over, SKY_OVERCAST_GREY * 0.8f * over };

	for(int l = 0; l < 2; l++) {
		float lum = luminance(layers[l]);

		for(int k = 0; k < 3; k++) {
			float c = layers[l][k] + (lum - layers[l][k]) * grey[l];

			layers[l][k] = c * dark + FLASH_COLOR[k] * weather->flash;
		}
	}
	for(int k = 0; k < 3; k++)
		sky->haze[k] = sky->horizon[k] * SKY_GROUND_HAZE;
	sky->star_amount *= 1.0f - smooth_step(0.1f, 0.6f, weather->cloud);
}

void sky_update(float hour) {
	sky_at(hour, &g_current);
	sky_apply_weather(&g_current, weather_now_get());
	g_current_set = 1;
}

const sky_state* sky_current(void) {
	if(!g_current_set)
		sky_update(12.0f);
	return &g_current;
}
