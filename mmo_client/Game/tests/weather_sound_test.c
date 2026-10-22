// Check the sound of the weather: it follows the sky, is dulled under a roof,
// and thunder comes after the lightning.
#include "audio/weather_sound.h"
#include "audio/mixer.h"
#include "world/weather_now.h"
#include "world/weather_state.h"
#include "protocol.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define STEP 0.05f
#define BLOCK ((int)(STEP * MIXER_RATE))

typedef struct {
	double now;
} clock_state;

// What the next start makes the weather: how hard it falls (for a wet day), and
// the wind level (-1 keeps the day's own)
static int g_fall = 3;
static int g_wind = -1;

// Where the player stands: a spot the weather reaches, found when the weather starts
static float g_px;
static float g_py;

// Run the weather and its sound for a while, returning the loudest 50 ms
static float run(clock_state* c, float seconds, int covered, float* mean) {
	int16_t block[BLOCK * 2];
	float loudest = 0.0f;
	double total = 0.0;
	int blocks = 0;

	for(float t = 0.0f; t < seconds; t += STEP) {
		double sum = 0.0;

		c->now += STEP;
		weather_state_tick(c->now);
		weather_now_update(STEP, 0, g_px, g_py);
		weather_sound_update(STEP, covered);
		mixer_render(block, BLOCK);
		for(int i = 0; i < BLOCK; i++) {
			float v = (float)block[i * 2] / 32768.0f;

			sum += (double)v * (double)v;
		}
		sum = sqrt(sum / (double)BLOCK);
		if(sum > loudest)
			loudest = (float)sum;
		total += sum;
		blocks++;
	}
	if(mean)
		*mean = (float)(total / (double)blocks);

	return loudest;
}

// Make the forced day fall as hard, and blow as hard, as the test says
static void apply_overrides(void) {
	WeatherDayData day;

	if(!weather_now_forced_day(&day))
		return;
	if(day.intensity > 0)
		day.intensity = (uint8_t)(day.condition == WEATHER_THUNDER && g_fall < 2 ? 2 : g_fall);
	if(g_wind >= 0)
		day.wind = (uint8_t)g_wind;
	weather_now_force_day(&day, 0);
}

static void start(int condition, clock_state* c) {
	weather_state_reset();
	weather_now_reset();
	mixer_init();
	weather_sound_init();
	weather_now_force(condition);
	apply_overrides();
	c->now = 1000.0;
	weather_state_tick(c->now);
	// Stand where it is falling hard (and stormy, in a storm)
	weather_now_update(0.0f, 0, 0.0f, 0.0f);
	g_px = 0.0f;
	g_py = 0.0f;
	for(int i = 0; i < 400; i++) {
		weather_cell_sample here;
		float x = (float)i * 130.0f - 20000.0f;

		weather_cells_sample(weather_now_cells(), x, g_py, &here);
		g_px = x;
		const weather_envelope* env = &weather_now_cells()->today;

		// Near the core of a patch of whatever falls, and of the storm in a storm
		if(here.rain >= 0.8f * env->rain && here.snow >= 0.8f * env->snow &&
		   here.storm >= 0.8f * env->storm)
			break;
	}
	// Starting again there, so the weather is already in place
	weather_now_force(condition);
	apply_overrides();
	weather_now_update(0.0f, 0, g_px, g_py);
}

static void finish(void) {
	g_fall = 3;
	g_wind = -1;
	weather_now_force(-1);
	weather_sound_shutdown();
	mixer_shutdown();
}

static void test_a_clear_day_is_quiet_and_rain_is_heard(void) {
	clock_state c;
	float clear_mean;
	float rain_mean;

	start(WEATHER_CLEAR, &c);
	run(&c, 3.0f, 0, NULL);
	run(&c, 2.0f, 0, &clear_mean);
	finish();
	start(WEATHER_RAIN, &c);
	run(&c, 3.0f, 0, NULL);
	run(&c, 2.0f, 0, &rain_mean);
	finish();
	assert(clear_mean < 0.01f);
	assert(rain_mean > 0.04f);
}

static void test_a_roof_dulls_the_rain(void) {
	clock_state c;
	float open_mean;
	float under_mean;

	start(WEATHER_RAIN, &c);
	run(&c, 3.0f, 0, NULL);
	run(&c, 2.0f, 0, &open_mean);
	run(&c, 3.0f, 1, NULL);
	run(&c, 2.0f, 1, &under_mean);
	finish();
	assert(under_mean < open_mean * 0.6f);
	assert(under_mean > 0.0f);
}

static void test_snow_falls_quietly_but_the_wind_blows(void) {
	clock_state c;
	float snow_mean;
	float rain_mean;

	start(WEATHER_SNOW, &c);
	run(&c, 3.0f, 0, NULL);
	run(&c, 2.0f, 0, &snow_mean);
	finish();
	start(WEATHER_RAIN, &c);
	run(&c, 3.0f, 0, NULL);
	run(&c, 2.0f, 0, &rain_mean);
	finish();
	assert(snow_mean < rain_mean * 0.6f);
}

static void test_silence_ends_the_weather_sound(void) {
	clock_state c;
	float after;

	start(WEATHER_THUNDER, &c);
	run(&c, 3.0f, 0, NULL);
	weather_sound_silence();
	// The bed is faded out; the weather itself is still going, but nothing
	// asks the sound to follow it until the next update
	{
		int16_t block[BLOCK * 2];
		double sum = 0.0;

		for(int i = 0; i < 40; i++)
			mixer_render(block, BLOCK);
		mixer_render(block, BLOCK);
		for(int i = 0; i < BLOCK; i++)
			sum += fabs((double)block[i * 2]);
		after = (float)(sum / BLOCK);
	}
	finish();
	assert(after < 1.0f);
}

static void test_thunder_follows_the_lightning(void) {
	clock_state c;
	float before_loudest;
	float after_loudest = 0.0f;
	int struck = 0;

	start(WEATHER_THUNDER, &c);
	// The rain settles in first
	before_loudest = run(&c, 4.0f, 0, NULL);
	// Wait for a strike, and then listen for the thunder that follows
	for(int i = 0; i < 4000 && !struck; i++) {
		before_loudest = fmaxf(before_loudest, run(&c, STEP, 0, NULL));
		struck = weather_now_get()->bolt > 0.0f;
	}
	assert(struck);
	for(int i = 0; i < (int)(5.0f / STEP); i++)
		after_loudest = fmaxf(after_loudest, run(&c, STEP, 0, NULL));
	finish();
	// The clap is a good deal louder than the rain it rolls over
	assert(after_loudest > before_loudest * 1.3f);
}

static void test_thunder_under_a_roof_is_softer(void) {
	clock_state c;
	float open_loudest = 0.0f;
	float under_loudest = 0.0f;

	for(int pass = 0; pass < 2; pass++) {
		int struck = 0;
		float loudest = 0.0f;

		start(WEATHER_THUNDER, &c);
		// The same stretch of sky both times, so the same strike
		c.now = 5000.0;
		run(&c, 4.0f, pass, NULL);
		for(int i = 0; i < 4000 && !struck; i++) {
			run(&c, STEP, pass, NULL);
			struck = weather_now_get()->bolt > 0.0f;
		}
		assert(struck);
		for(int i = 0; i < (int)(5.0f / STEP); i++)
			loudest = fmaxf(loudest, run(&c, STEP, pass, NULL));
		if(pass == 0)
			open_loudest = loudest;
		else
			under_loudest = loudest;
		finish();
	}
	assert(under_loudest < open_loudest * 0.75f);
}

static void test_the_volume_settings_scale_each_sound(void) {
	clock_state c;
	float full;
	float half;
	float none;
	float no_wind_full;
	float no_wind_rain_quiet;

	start(WEATHER_RAIN, &c);
	weather_sound_set_volumes(1.0f, 1.0f, 1.0f, 1.0f);
	run(&c, 3.0f, 0, NULL);
	run(&c, 2.0f, 0, &full);
	// The weather volume is the master of the others
	weather_sound_set_volumes(0.5f, 1.0f, 1.0f, 1.0f);
	run(&c, 3.0f, 0, NULL);
	run(&c, 2.0f, 0, &half);
	weather_sound_set_volumes(0.0f, 1.0f, 1.0f, 1.0f);
	run(&c, 3.0f, 0, NULL);
	run(&c, 2.0f, 0, &none);
	// Wind alone, and then rain alone, of the same storm
	weather_sound_set_volumes(1.0f, 0.0f, 1.0f, 1.0f);
	run(&c, 3.0f, 0, NULL);
	run(&c, 2.0f, 0, &no_wind_full);
	weather_sound_set_volumes(1.0f, 1.0f, 0.0f, 1.0f);
	run(&c, 3.0f, 0, NULL);
	run(&c, 2.0f, 0, &no_wind_rain_quiet);
	finish();
	weather_sound_set_volumes(1.0f, 1.0f, 1.0f, 1.0f);
	assert(half > full * 0.35f && half < full * 0.65f);
	assert(none < 0.0005f);
	// Rain off leaves less than rain on, and wind off leaves less than both on
	assert(no_wind_full < full);
	assert(no_wind_rain_quiet < full);
}

static void test_the_thunder_volume_silences_thunder(void) {
	clock_state c;
	float loud = 0.0f;
	float quiet = 0.0f;

	for(int pass = 0; pass < 2; pass++) {
		int struck = 0;
		float loudest = 0.0f;

		start(WEATHER_THUNDER, &c);
		weather_sound_set_volumes(1.0f, 0.0f, 0.0f, pass ? 0.0f : 1.0f);
		c.now = 5000.0;
		run(&c, 4.0f, 0, NULL);
		for(int i = 0; i < 4000 && !struck; i++) {
			run(&c, STEP, 0, NULL);
			struck = weather_now_get()->bolt > 0.0f;
		}
		assert(struck);
		for(int i = 0; i < (int)(5.0f / STEP); i++)
			loudest = fmaxf(loudest, run(&c, STEP, 0, NULL));
		if(pass == 0)
			loud = loudest;
		else
			quiet = loudest;
		finish();
	}
	weather_sound_set_volumes(1.0f, 1.0f, 1.0f, 1.0f);
	assert(loud > 0.05f);
	assert(quiet < 0.001f);
}

static void test_a_harder_rain_is_louder(void) {
	clock_state c;
	float light_mean;
	float heavy_mean;

	g_fall = 1;
	start(WEATHER_RAIN, &c);
	run(&c, 3.0f, 0, NULL);
	run(&c, 2.0f, 0, &light_mean);
	finish();
	g_fall = 3;
	start(WEATHER_RAIN, &c);
	run(&c, 3.0f, 0, NULL);
	run(&c, 2.0f, 0, &heavy_mean);
	finish();
	assert(light_mean > 0.005f);
	assert(heavy_mean > light_mean * 1.5f);
}

static void test_a_stronger_wind_is_louder(void) {
	clock_state c;
	float calm_mean;
	float breeze_mean;
	float gale_mean;

	g_wind = 0;
	start(WEATHER_CLOUDY, &c);
	run(&c, 6.0f, 0, NULL);
	run(&c, 3.0f, 0, &calm_mean);
	finish();
	g_wind = 1;
	start(WEATHER_CLOUDY, &c);
	run(&c, 6.0f, 0, NULL);
	run(&c, 3.0f, 0, &breeze_mean);
	finish();
	g_wind = 3;
	start(WEATHER_CLOUDY, &c);
	run(&c, 6.0f, 0, NULL);
	run(&c, 3.0f, 0, &gale_mean);
	finish();
	assert(calm_mean < breeze_mean);
	assert(breeze_mean < gale_mean);
	assert(gale_mean > 0.03f);
}

int main(void) {
	test_a_harder_rain_is_louder();
	test_a_stronger_wind_is_louder();
	test_the_volume_settings_scale_each_sound();
	test_the_thunder_volume_silences_thunder();
	test_a_clear_day_is_quiet_and_rain_is_heard();
	test_a_roof_dulls_the_rain();
	test_snow_falls_quietly_but_the_wind_blows();
	test_silence_ends_the_weather_sound();
	test_thunder_follows_the_lightning();
	test_thunder_under_a_roof_is_softer();
	printf("weather_sound_test: all passed\n");

	return 0;
}
