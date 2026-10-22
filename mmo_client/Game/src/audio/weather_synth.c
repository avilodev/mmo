// Rain, wind and thunder made from noise.

#include "audio/weather_synth.h"

#include <math.h>
#include <stdlib.h>

#define PI_F 3.14159265f

// Seconds the end of a loop is blended into its start, so it has no seam
#define LOOP_FADE_S 0.4f

// The level the sounds are made at: their loudness before the mixer's gains
#define RAIN_RMS 0.14f
#define WIND_RMS 0.12f
#define THUNDER_PEAK 0.9f

typedef struct {
	unsigned int state;
} noise_source;

// White noise from -1 to 1
static float noise_next(noise_source* n) {
	n->state ^= n->state << 13;
	n->state ^= n->state >> 17;
	n->state ^= n->state << 5;

	return (float)(n->state & 0xFFFFFF) / 8388608.0f - 1.0f;
}

// A number from 0 to 1
static float unit_next(noise_source* n) {
	return noise_next(n) * 0.5f + 0.5f;
}

static int16_t to_sample(float v) {
	if(v > 1.0f)
		v = 1.0f;
	if(v < -1.0f)
		v = -1.0f;

	return (int16_t)lroundf(v * 32767.0f);
}

static float rms_of(const float* v, int count) {
	double sum = 0.0;

	for(int i = 0; i < count; i++)
		sum += (double)v[i] * (double)v[i];

	return (float)sqrt(sum / (double)(count > 0 ? count : 1));
}

// Turn a run of frames + fade samples into a loop of frames, at a loudness
static void make_loop(const float* run, int frames, int fade, float rms, int16_t* out) {
	float scale = rms / (rms_of(run, frames) + 1.0e-9f);

	for(int i = 0; i < frames; i++) {
		float v = run[i];

		// The start is blended with the samples that would follow the end
		if(i < fade) {
			float t = (float)i / (float)fade * PI_F * 0.5f;

			v = run[i] * sinf(t) + run[frames + i] * cosf(t);
		}
		out[i] = to_sample(v * scale);
	}
}

// What makes each kind of rain what it is
typedef struct {
	float hiss; // how much of the high hiss
	float body; // how much of the lower body of the noise
	float tick_chance; // chance a sample starts a drop's click
	float tick_level; // how loud the clicks are
	float roar; // how much low rumble there is under it all
} rain_voicing;

static const rain_voicing RAIN_VOICES[3] = {
	{ 0.45f, 0.30f, 0.0006f, 1.4f, 0.0f },
	{ 0.60f, 0.80f, 0.0015f, 1.0f, 0.4f },
	{ 0.85f, 1.25f, 0.0045f, 0.8f, 2.2f }
};

void weather_synth_rain_kind(int16_t* out, int frames, weather_rain_kind kind) {
	const rain_voicing* voicing = &RAIN_VOICES[kind < 0 || kind > 2 ? 1 : kind];
	int fade = (int)(LOOP_FADE_S * WEATHER_SYNTH_RATE);
	int total = frames + fade;
	float* run = malloc((size_t)total * sizeof(float));
	noise_source n = { 2463534242u + (unsigned int)kind * 977u };
	float low = 0.0f;
	float soft = 0.0f;
	float tick = 0.0f;
	float rumble_a = 0.0f;
	float rumble_b = 0.0f;

	if(!run)
		return;
	for(int i = 0; i < total; i++) {
		float t = (float)i / WEATHER_SYNTH_RATE;
		float x = noise_next(&n);
		float v;

		// Keep what is above a few hundred hertz, and a softer copy for body
		low += 0.18f * (x - low);
		v = x - low;
		soft += 0.5f * (v - soft);
		// The low roar of a lot of rain at once: noise with only its bottom kept
		rumble_a += 0.07f * (x - rumble_a);
		rumble_b += 0.07f * (rumble_a - rumble_b);
		v = voicing->hiss * v + voicing->body * soft + voicing->roar * 6.0f * rumble_b;
		// Fine patter: a drop now and then, a click a millisecond long
		if(unit_next(&n) > 1.0f - voicing->tick_chance)
			tick = (0.6f + 0.8f * unit_next(&n)) * voicing->tick_level;
		v += tick * noise_next(&n);
		tick *= 0.9f;
		v *= 1.0f + 0.06f * sinf(t * 2.0f * PI_F / 3.1f) + 0.05f * sinf(t * 2.0f * PI_F / 1.7f);
		run[i] = v;
	}
	make_loop(run, frames, fade, RAIN_RMS, out);
	free(run);
}

void weather_synth_rain(int16_t* out, int frames) {
	weather_synth_rain_kind(out, frames, RAIN_MODERATE);
}

void weather_synth_wind_gale(int16_t* out, int frames, int gale) {
	int fade = (int)(LOOP_FADE_S * WEATHER_SYNTH_RATE);
	int total = frames + fade;
	float* run = malloc((size_t)total * sizeof(float));
	noise_source n = { gale ? 3141592653u : 88172645u };
	float band = 0.0f;
	float lowpass = 0.0f;
	// A gale howls higher and rises and falls harder, and rings more
	float base_pitch = gale ? 420.0f : 260.0f;
	float swing_pitch = gale ? 380.0f : 200.0f;
	float ring = gale ? 0.12f : 0.25f;
	float gust_depth = gale ? 0.9f : 0.45f;

	if(!run)
		return;
	for(int i = 0; i < total; i++) {
		float t = (float)i / WEATHER_SYNTH_RATE;
		// The howl's pitch wanders, and it comes in gusts
		float pitch = base_pitch + swing_pitch * (0.5f + 0.5f * sinf(t * 2.0f * PI_F / 5.3f)) +
		              swing_pitch * 0.55f * sinf(t * 2.0f * PI_F / 2.3f);
		float f = 2.0f * sinf(PI_F * pitch / WEATHER_SYNTH_RATE);
		float gust = 0.55f + gust_depth * sinf(t * 2.0f * PI_F / 7.1f + 1.3f) *
		             sinf(t * 2.0f * PI_F / 3.7f);
		float x = noise_next(&n);
		float high;

		// A resonant filter on noise: a whistle with a breath in it
		lowpass += f * band;
		high = x - lowpass - ring * band;
		band += f * high;
		run[i] = band * (0.4f + gust);
	}
	make_loop(run, frames, fade, WIND_RMS, out);
	free(run);
}

void weather_synth_wind(int16_t* out, int frames) {
	weather_synth_wind_gale(out, frames, 0);
}

void weather_synth_thunder(int16_t* out, int frames, unsigned int seed) {
	noise_source n = { 2654435761u * (seed + 7u) + 12345u };
	float* run = malloc((size_t)frames * sizeof(float));
	float low1 = 0.0f;
	float low2 = 0.0f;
	float mid = 0.0f;
	float peak = 1.0e-9f;
	float decay = 1.6f + 1.0f * unit_next(&n);
	// How sharp the crack is: the nearest has a hard one, the farthest none
	float crack = seed % 3u == 0u ? 1.0f : (seed % 3u == 1u ? 0.35f : 0.0f);
	float swell_at[4];
	float swell_amp[4];
	float swell_decay[4];
	int taper = (int)(0.3f * WEATHER_SYNTH_RATE);

	if(!run)
		return;
	for(int k = 0; k < 4; k++) {
		swell_at[k] = 0.5f + 2.8f * unit_next(&n);
		swell_amp[k] = 0.25f + 0.5f * unit_next(&n);
		swell_decay[k] = 0.5f + 0.7f * unit_next(&n);
	}
	for(int i = 0; i < frames; i++) {
		float t = (float)i / WEATHER_SYNTH_RATE;
		float x = noise_next(&n);
		// The rumble's pitch drops as it rolls away
		float a = 2.0f * PI_F * (100.0f + 240.0f * expf(-t / 2.5f)) / WEATHER_SYNTH_RATE;
		float env = (1.0f - expf(-t / 0.12f)) * expf(-t / decay);
		float rumble;
		float tear;
		float bang;

		for(int k = 0; k < 4; k++)
			if(t > swell_at[k])
				env += swell_amp[k] * (1.0f - expf(-(t - swell_at[k]) / 0.15f)) *
				       expf(-(t - swell_at[k]) / swell_decay[k]);
		low1 += a * (x - low1);
		low2 += a * (low1 - low2);
		rumble = low2 * 9.0f * env;
		// The crack is bright noise that dies in a tenth of a second, and a
		// tearing crackle that follows it
		mid += 0.35f * (x - mid);
		bang = (x - mid) * expf(-t / 0.04f) * (1.0f - expf(-t / 0.002f));
		tear = mid * expf(-t / 0.25f) * 0.35f * (1.0f - expf(-t / 0.01f));
		run[i] = rumble + crack * (bang * 1.2f + tear);
		if(i >= frames - taper)
			run[i] *= (float)(frames - 1 - i) / (float)taper;
		if(fabsf(run[i]) > peak)
			peak = fabsf(run[i]);
	}
	for(int i = 0; i < frames; i++)
		out[i] = to_sample(run[i] / peak * THUNDER_PEAK);
	free(run);
}
