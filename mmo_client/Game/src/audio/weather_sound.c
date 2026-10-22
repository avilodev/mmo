// The sound of the weather.

#include "audio/weather_sound.h"
#include "audio/mixer.h"
#include "audio/weather_synth.h"
#include "world/weather_now.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>

#define RAIN_FRAMES (WEATHER_SYNTH_RATE * 4)
#define WIND_FRAMES (WEATHER_SYNTH_RATE * 6)
#define THUNDER_FRAMES (WEATHER_SYNTH_RATE * 6)
#define THUNDER_VARIANTS 3

// How loud the rain is at its hardest, and the wind
#define RAIN_LEVEL 0.45f
#define WIND_LEVEL 0.45f

// Wind speed, world units a second, that counts as a full gale
#define GALE_SPEED 120.0f

// Under a roof the rain is this much quieter and duller, and the thunder
#define COVER_RAIN_QUIET 0.65f
#define COVER_MUFFLE 0.9f
#define COVER_THUNDER_QUIET 0.4f

// Seconds the sound takes to move to under cover and out again
#define COVER_EASE_S 0.7f

// Thunder comes this long after the flash, at the soonest and at the latest
#define THUNDER_DELAY_MIN_S 0.35f
#define THUNDER_DELAY_SPAN_S 3.4f

#define MAX_PENDING 4

typedef struct {
	float wait;
	float gain;
	int variant;
} pending_thunder;

// The rain and wind beds, from soft to hard, each with its voice in the mixer
#define RAIN_BEDS 3
#define WIND_BEDS 2

static int16_t* g_rain[RAIN_BEDS];
static int16_t* g_wind[WIND_BEDS];
static int16_t* g_thunder[THUNDER_VARIANTS];
static int g_rain_voice[RAIN_BEDS];
static int g_wind_voice[WIND_BEDS];
static pending_thunder g_pending[MAX_PENDING];
static float g_cover;
static float g_last_bolt;
// The player's levels: weather, rain, wind, thunder
static float g_weather_level = 1.0f;
static float g_rain_level = 1.0f;
static float g_wind_level = 1.0f;
static float g_thunder_level = 1.0f;

static float clamp01(float v) {
	if(v < 0.0f)
		return 0.0f;

	return v > 1.0f ? 1.0f : v;
}

void weather_sound_set_volumes(float weather, float rain, float wind, float thunder) {
	g_weather_level = clamp01(weather);
	g_rain_level = clamp01(rain);
	g_wind_level = clamp01(wind);
	g_thunder_level = clamp01(thunder);
}

void weather_sound_init(void) {
	weather_sound_shutdown();
	for(int b = 0; b < RAIN_BEDS; b++) {
		g_rain[b] = malloc(RAIN_FRAMES * sizeof(int16_t));
		if(!g_rain[b])
			return;
		weather_synth_rain_kind(g_rain[b], RAIN_FRAMES, (weather_rain_kind)b);
		g_rain_voice[b] = mixer_loop_add(g_rain[b], RAIN_FRAMES);
	}
	for(int b = 0; b < WIND_BEDS; b++) {
		g_wind[b] = malloc(WIND_FRAMES * sizeof(int16_t));
		if(!g_wind[b])
			return;
		weather_synth_wind_gale(g_wind[b], WIND_FRAMES, b);
		g_wind_voice[b] = mixer_loop_add(g_wind[b], WIND_FRAMES);
	}
	for(int v = 0; v < THUNDER_VARIANTS; v++) {
		g_thunder[v] = malloc(THUNDER_FRAMES * sizeof(int16_t));
		if(g_thunder[v])
			weather_synth_thunder(g_thunder[v], THUNDER_FRAMES, (unsigned int)v);
	}
}

void weather_sound_shutdown(void) {
	mixer_silence();
	for(int b = 0; b < RAIN_BEDS; b++) {
		free(g_rain[b]);
		g_rain[b] = NULL;
		g_rain_voice[b] = -1;
	}
	for(int b = 0; b < WIND_BEDS; b++) {
		free(g_wind[b]);
		g_wind[b] = NULL;
		g_wind_voice[b] = -1;
	}
	for(int v = 0; v < THUNDER_VARIANTS; v++) {
		free(g_thunder[v]);
		g_thunder[v] = NULL;
	}
	for(int i = 0; i < MAX_PENDING; i++)
		g_pending[i].wait = -1.0f;
	g_cover = 0.0f;
	g_last_bolt = 0.0f;
}

void weather_sound_silence(void) {
	mixer_silence();
	for(int i = 0; i < MAX_PENDING; i++)
		g_pending[i].wait = -1.0f;
}

// A number from 0 to 1 that is always the same for a strike's seed
static float seed_unit(float seed, float salt) {
	unsigned int h = (unsigned int)(seed * 16777216.0f) * 2654435761u +
	                 (unsigned int)(salt * 977.0f) * 40503u;

	h ^= h >> 15;
	h *= 2246822519u;
	h ^= h >> 13;

	return (float)(h & 0xFFFFFF) / 16777216.0f;
}

// A new flash has begun: thunder follows, later the further away the strike is
static void schedule_thunder(const weather_now* weather) {
	float far = seed_unit(weather->bolt_seed, 1.0f);
	int slot = -1;

	for(int i = 0; i < MAX_PENDING; i++)
		if(g_pending[i].wait < 0.0f) {
			slot = i;
			break;
		}
	if(slot < 0)
		return;
	// A storm that is not overhead is further off: its thunder is later and softer
	float here = clamp01(weather->storm * 2.0f);

	g_pending[slot].wait = THUNDER_DELAY_MIN_S + THUNDER_DELAY_SPAN_S * far + (1.0f - here) * 1.5f;
	// The nearest strikes are the loud ones, and the sharpest
	g_pending[slot].gain = (1.0f - 0.45f * far) * (0.45f + 0.55f * here);
	g_pending[slot].variant = far < 0.33f ? 0 : (far < 0.66f ? 1 : 2);
}

static void play_due_thunder(float dt) {
	for(int i = 0; i < MAX_PENDING; i++) {
		pending_thunder* t = &g_pending[i];

		if(t->wait < 0.0f)
			continue;
		t->wait -= dt;
		if(t->wait > 0.0f)
			continue;
		t->wait = -1.0f;
		if(g_thunder[t->variant])
			mixer_play(g_thunder[t->variant], THUNDER_FRAMES,
			           t->gain * (1.0f - COVER_THUNDER_QUIET * g_cover) * g_thunder_level *
			           g_weather_level, g_cover * COVER_MUFFLE * 0.6f);
	}
}

static float smooth_step(float lo, float hi, float x) {
	float t = clamp01((x - lo) / (hi - lo));

	return t * t * (3.0f - 2.0f * t);
}

void weather_sound_update(float dt, int covered) {
	const weather_now* weather = weather_now_get();
	float step = dt / COVER_EASE_S;
	float speed = sqrtf(weather->wind[0] * weather->wind[0] + weather->wind[1] * weather->wind[1]);
	float quiet = (1.0f - COVER_RAIN_QUIET * g_cover) * g_weather_level;
	float muffle = g_cover * COVER_MUFFLE;
	float rain;
	float wind;
	float light;
	float heavy;
	float gale;

	if(dt < 0.0f)
		dt = 0.0f;
	g_cover += ((covered ? 1.0f : 0.0f) - g_cover) * (step > 1.0f ? 1.0f : step);

	// Rain is heard well before it is hard. Which bed it is changes with how hard:
	// a few drops, then steady rain, then a roar, each fading into the next.
	rain = powf(clamp01(weather->rain), 0.8f) * 1.1f * RAIN_LEVEL * quiet * g_rain_level;
	light = 1.0f - smooth_step(0.15f, 0.45f, weather->rain);
	heavy = smooth_step(0.55f, 0.85f, weather->rain);
	mixer_loop_set(g_rain_voice[0], rain * light, muffle);
	mixer_loop_set(g_rain_voice[1], rain * clamp01(1.0f - light - heavy), muffle);
	mixer_loop_set(g_rain_voice[2], rain * heavy, muffle);

	// Wind is a breeze's soft sigh until it is a gale's howl; snow falls without
	// a sound, but with a hush; rain brings a little wind of its own
	wind = clamp01(speed / GALE_SPEED) * WIND_LEVEL + weather->snow * 0.12f + weather->rain * 0.08f;
	wind *= (1.0f - 0.5f * g_cover) * g_weather_level * g_wind_level;
	gale = smooth_step(1.4f, 2.6f, weather->wind_level);
	mixer_loop_set(g_wind_voice[0], wind * (1.0f - gale), g_cover * 0.6f);
	mixer_loop_set(g_wind_voice[1], wind * gale, g_cover * 0.6f);

	// A bolt that was not there a moment ago is a new strike
	if(weather->bolt > 0.0f && g_last_bolt <= 0.0f)
		schedule_thunder(weather);
	g_last_bolt = weather->bolt;
	play_due_thunder(dt);
}
