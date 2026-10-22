// How the world is lit, from the sky.

#include "world/world_lighting.h"

#include <math.h>

// Brightness of the sun and the moon as lights
#define SUN_BRIGHTNESS 0.55f
#define MOON_BRIGHTNESS 0.28f
// The moon casts weaker shadows than the sun
#define MOON_SHADOW_STRENGTH 0.5f

static const float MOON_COLOR[3] = { 0.60f, 0.70f, 1.00f };
// Noon: with the sun they sum to 1 on a surface facing up
static const float DAY_UP[3] = { 0.49f, 0.51f, 0.57f };
static const float DAY_DOWN[3] = { 0.38f, 0.36f, 0.32f };
static const float NIGHT_UP[3] = { 0.10f, 0.12f, 0.22f };
static const float NIGHT_DOWN[3] = { 0.05f, 0.06f, 0.10f };

static world_lighting g_current;
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

// The direction of the big light: the sun, handing over to the moon at dusk
static void light_direction(const sky_state* sky, float* out) {
	float w = smooth_step(-0.12f, -0.02f, sky->sun_dir[1]);
	float len;

	for(int i = 0; i < 3; i++)
		out[i] = sky->moon_dir[i] + (sky->sun_dir[i] - sky->moon_dir[i]) * w;
	len = sqrtf(out[0] * out[0] + out[1] * out[1] + out[2] * out[2]);
	if(len < 1.0e-3f) {
		out[0] = 0.0f;
		out[1] = 1.0f;
		out[2] = 0.0f;
		return;
	}
	for(int i = 0; i < 3; i++)
		out[i] /= len;
}

void world_lighting_from_sky(const sky_state* sky, world_lighting* out) {
	float sun_y = sky->sun_dir[1];
	float sun = smooth_step(-0.10f, 0.20f, sun_y);
	float moon = smooth_step(-0.05f, 0.15f, sky->moon_dir[1]);
	// Daylight fades over the whole of twilight, as the sky does
	float day = smooth_step(-0.40f, 0.15f, sun_y);
	float moon_shadow = MOON_SHADOW_STRENGTH * smooth_step(0.05f, 0.25f, sky->moon_dir[1]);
	float sun_shadow = smooth_step(0.02f, 0.20f, sun_y);

	light_direction(sky, out->dir);
	for(int i = 0; i < 3; i++)
		out->color[i] = SUN_BRIGHTNESS * sun * sky->sun_color[i] +
		                MOON_BRIGHTNESS * moon * MOON_COLOR[i];
	mix3(NIGHT_UP, DAY_UP, day, out->ambient_up);
	mix3(NIGHT_DOWN, DAY_DOWN, day, out->ambient_down);
	out->shadow_strength = sun_shadow > moon_shadow ? sun_shadow : moon_shadow;
	out->lamp_amount = 1.0f - smooth_step(-0.20f, 0.30f, sun_y);
}

void world_lighting_noon(world_lighting* out) {
	sky_state sky;

	sky_at(12.0f, &sky);
	world_lighting_from_sky(&sky, out);
	out->shadow_strength = 0.0f;
	out->lamp_amount = 0.0f;
}

void world_lighting_surface(const world_lighting* light, const float normal[3], float out[3]) {
	float d = normal[0] * light->dir[0] + normal[1] * light->dir[1] + normal[2] * light->dir[2];
	float wrap = d * 0.5f + 0.5f;
	float up = normal[1] * 0.5f + 0.5f;
	float ambient[3];

	mix3(light->ambient_down, light->ambient_up, up, ambient);
	for(int i = 0; i < 3; i++)
		out[i] = ambient[i] + light->color[i] * wrap * wrap;
}

// How much of the big light is left under full overcast, and in a storm
#define OVERCAST_LIGHT WORLD_LIGHTING_OVERCAST_SUN
#define STORM_LIGHT 0.15f
// How much the ambient light greys and dims under full overcast
#define OVERCAST_GREY 0.5f
#define OVERCAST_AMBIENT 0.18f
#define STORM_AMBIENT 0.12f
// Snow lights the underside of everything
#define SNOW_BOUNCE 0.5f
#define SNOW_FALL_BOUNCE 0.2f

// How bright the lamps burn in the gloom of a storm at noon, at most
#define GLOOM_LAMPS 0.85f

static const float FLASH_LIGHT[3] = { 0.45f, 0.50f, 0.70f };

void world_lighting_apply_weather(world_lighting* light, const weather_now* weather) {
	float over = smooth_step(0.3f, 1.0f, weather->cloud);
	float dim = 1.0f - (1.0f - OVERCAST_LIGHT) * over - STORM_LIGHT * weather->storm * over;
	float ambient = 1.0f - OVERCAST_AMBIENT * over - STORM_AMBIENT * weather->storm * over;
	float gloom;
	float* layers[2] = { light->ambient_up, light->ambient_down };

	ambient *= 1.0f + SNOW_BOUNCE * weather->snow_cover + SNOW_FALL_BOUNCE * weather->snow;
	// Under a heavy sky the lamps come on, the harder it storms the brighter
	gloom = smooth_step(0.7f, 1.0f, weather->cloud) *
	        clamp01(0.3f + 0.7f * weather->storm + 0.3f * weather->rain) * GLOOM_LAMPS;
	if(gloom > light->lamp_amount)
		light->lamp_amount = gloom;
	for(int i = 0; i < 3; i++)
		light->color[i] *= dim;
	light->shadow_strength *= 1.0f - smooth_step(0.3f, 0.8f, weather->cloud);
	for(int l = 0; l < 2; l++) {
		float lum = 0.30f * layers[l][0] + 0.59f * layers[l][1] + 0.11f * layers[l][2];

		for(int i = 0; i < 3; i++) {
			float c = layers[l][i] + (lum - layers[l][i]) * OVERCAST_GREY * over;

			layers[l][i] = c * ambient + FLASH_LIGHT[i] * weather->flash;
		}
	}
}

void world_lighting_update(const sky_state* sky) {
	world_lighting_from_sky(sky, &g_current);
	world_lighting_apply_weather(&g_current, weather_now_get());
	g_current_set = 1;
}

const world_lighting* world_lighting_current(void) {
	if(!g_current_set)
		world_lighting_noon(&g_current);
	return &g_current;
}
