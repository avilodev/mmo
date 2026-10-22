// Check the weather sounds: they are loud enough, never clip, loop without a
// seam, die away, and come out the same each time.
#include "audio/weather_synth.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RATE WEATHER_SYNTH_RATE

static float rms(const int16_t* s, int from, int to) {
	double sum = 0.0;

	for(int i = from; i < to; i++)
		sum += ((double)s[i] / 32768.0) * ((double)s[i] / 32768.0);

	return (float)sqrt(sum / (double)(to - from));
}

static int peak(const int16_t* s, int from, int to) {
	int best = 0;

	for(int i = from; i < to; i++)
		if(abs(s[i]) > best)
			best = abs(s[i]);

	return best;
}

// The mean step between neighbouring samples, to judge a seam against
static float mean_step(const int16_t* s, int count) {
	double sum = 0.0;

	for(int i = 1; i < count; i++)
		sum += fabs((double)s[i] - (double)s[i - 1]);

	return (float)(sum / (double)(count - 1));
}

static void test_rain_is_steady_and_loops(void) {
	int frames = RATE * 4;
	int16_t* a = malloc((size_t)frames * sizeof(int16_t));
	int16_t* b = malloc((size_t)frames * sizeof(int16_t));
	float level;

	weather_synth_rain(a, frames);
	weather_synth_rain(b, frames);
	assert(memcmp(a, b, (size_t)frames * sizeof(int16_t)) == 0);
	level = rms(a, 0, frames);
	assert(level > 0.10f && level < 0.18f);
	assert(peak(a, 0, frames) < 32000);
	// Every second is about as loud as the whole
	for(int s = 0; s < 4; s++) {
		float part = rms(a, s * RATE, (s + 1) * RATE);

		assert(part > level * 0.8f && part < level * 1.25f);
	}
	// The join of the end to the start is no bigger a step than any other
	assert(fabsf((float)a[0] - (float)a[frames - 1]) < mean_step(a, frames) * 8.0f);
	free(a);
	free(b);
}

static void test_wind_is_low_and_loops(void) {
	int frames = RATE * 6;
	int16_t* a = malloc((size_t)frames * sizeof(int16_t));
	float level;

	weather_synth_wind(a, frames);
	level = rms(a, 0, frames);
	assert(level > 0.09f && level < 0.16f);
	assert(peak(a, 0, frames) < 32000);
	// Smooth, a howl and not a hiss: neighbouring samples are close
	assert(mean_step(a, frames) < 2500.0f);
	assert(fabsf((float)a[0] - (float)a[frames - 1]) < mean_step(a, frames) * 8.0f + 200.0f);
	free(a);
}

static void test_thunder_cracks_rolls_and_dies(void) {
	int frames = RATE * 6;
	int16_t* near_clap = malloc((size_t)frames * sizeof(int16_t));
	int16_t* far_clap = malloc((size_t)frames * sizeof(int16_t));
	int16_t* again = malloc((size_t)frames * sizeof(int16_t));

	weather_synth_thunder(near_clap, frames, 0);
	weather_synth_thunder(far_clap, frames, 2);
	weather_synth_thunder(again, frames, 0);
	assert(memcmp(near_clap, again, (size_t)frames * sizeof(int16_t)) == 0);
	assert(memcmp(near_clap, far_clap, (size_t)frames * sizeof(int16_t)) != 0);
	// It is loud, and does not clip
	assert(peak(near_clap, 0, frames) > 20000 && peak(near_clap, 0, frames) < 32768);
	assert(peak(far_clap, 0, frames) > 20000);
	// It starts quietly enough to have no click, and ends in silence
	assert(abs(near_clap[0]) < 2000);
	assert(peak(near_clap, frames - RATE / 10, frames) < 600);
	assert(peak(far_clap, frames - RATE / 10, frames) < 600);
	// The near one opens with a crack: its first tenth of a second is harder
	// than the far one's, which only builds
	assert(rms(near_clap, 0, RATE / 10) > rms(far_clap, 0, RATE / 10) * 1.5f);
	// A rumble goes on for seconds
	assert(rms(far_clap, RATE, RATE * 2) > 0.02f);
	free(near_clap);
	free(far_clap);
	free(again);
}

static void test_the_far_clap_is_low(void) {
	int frames = RATE * 6;
	int16_t* far_clap = malloc((size_t)frames * sizeof(int16_t));
	double changes = 0.0;
	double size = 0.0;

	weather_synth_thunder(far_clap, frames, 2);
	for(int i = 1; i < frames; i++) {
		changes += fabs((double)far_clap[i] - (double)far_clap[i - 1]);
		size += fabs((double)far_clap[i]);
	}
	// A low sound changes little from one sample to the next, against its size
	assert(changes / size < 0.35);
	free(far_clap);
}

// How much of a sound's energy is low (under a few hundred hertz), 0..1
static float low_share(const int16_t* s, int frames) {
	double low = 0.0;
	double all = 0.0;
	float state = 0.0f;

	for(int i = 0; i < frames; i++) {
		float x = (float)s[i] / 32768.0f;

		state += 0.06f * (x - state);
		low += (double)state * (double)state;
		all += (double)x * (double)x;
	}

	return (float)(low / all);
}

// The loudest sample against the average: how spiky a sound is
static float crest_of(const int16_t* s, int frames) {
	return (float)peak(s, 0, frames) / 32768.0f / rms(s, 0, frames);
}

// How many times a sound crosses zero: more for a higher sound
static int crossings(const int16_t* s, int frames) {
	int count = 0;

	for(int i = 1; i < frames; i++)
		if((s[i] >= 0) != (s[i - 1] >= 0))
			count++;

	return count;
}

static void test_rain_gets_heavier(void) {
	int frames = RATE * 4;
	int16_t* light = malloc((size_t)frames * sizeof(int16_t));
	int16_t* moderate = malloc((size_t)frames * sizeof(int16_t));
	int16_t* heavy = malloc((size_t)frames * sizeof(int16_t));

	weather_synth_rain_kind(light, frames, RAIN_LIGHT);
	weather_synth_rain_kind(moderate, frames, RAIN_MODERATE);
	weather_synth_rain_kind(heavy, frames, RAIN_HEAVY);
	// The moderate one is the rain there always was
	{
		int16_t* old = malloc((size_t)frames * sizeof(int16_t));

		weather_synth_rain(old, frames);
		assert(memcmp(old, moderate, (size_t)frames * sizeof(int16_t)) == 0);
		free(old);
	}
	// All three are about as loud, so the mixer's gain is what makes rain louder
	assert(rms(light, 0, frames) > 0.10f && rms(light, 0, frames) < 0.18f);
	assert(rms(heavy, 0, frames) > 0.10f && rms(heavy, 0, frames) < 0.18f);
	assert(peak(light, 0, frames) < 32000 && peak(heavy, 0, frames) < 32000);
	// A light rain is sparse drops on a hiss, so it is spikier than the roar of a heavy one
	assert(crest_of(light, frames) > crest_of(heavy, frames) * 1.15f);
	// A heavy rain has the body of a roar in it
	assert(low_share(heavy, frames) > low_share(light, frames) * 1.5f);
	// They are different sounds
	assert(memcmp(light, heavy, (size_t)frames * sizeof(int16_t)) != 0);
	// Each loops without a seam
	assert(fabsf((float)light[0] - (float)light[frames - 1]) < mean_step(light, frames) * 8.0f);
	assert(fabsf((float)heavy[0] - (float)heavy[frames - 1]) < mean_step(heavy, frames) * 8.0f);
	free(light);
	free(moderate);
	free(heavy);
}

static void test_a_gale_howls_higher_than_a_breeze(void) {
	int frames = RATE * 6;
	int16_t* breeze = malloc((size_t)frames * sizeof(int16_t));
	int16_t* gale = malloc((size_t)frames * sizeof(int16_t));
	int16_t* again = malloc((size_t)frames * sizeof(int16_t));

	weather_synth_wind_gale(breeze, frames, 0);
	weather_synth_wind_gale(gale, frames, 1);
	weather_synth_wind_gale(again, frames, 1);
	assert(memcmp(gale, again, (size_t)frames * sizeof(int16_t)) == 0);
	assert(rms(gale, 0, frames) > 0.08f && rms(gale, 0, frames) < 0.18f);
	assert(peak(gale, 0, frames) < 32000);
	// Higher: less of it is low, and it crosses zero more often
	assert(low_share(gale, frames) < low_share(breeze, frames));
	assert(crossings(gale, frames) > crossings(breeze, frames));
	free(breeze);
	free(gale);
	free(again);
}

int main(void) {
	test_rain_is_steady_and_loops();
	test_wind_is_low_and_loops();
	test_thunder_cracks_rolls_and_dies();
	test_the_far_clap_is_low();
	test_rain_gets_heavier();
	test_a_gale_howls_higher_than_a_breeze();
	printf("weather_synth_test: all passed\n");

	return 0;
}
