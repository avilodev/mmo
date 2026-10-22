// Check the mixer: silence, gains, muffling, one-shots and fading out.
#include "audio/mixer.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define BLOCK 512

// A loop of noise, so the muffling has something to take the highs from
static int16_t* make_noise(int frames) {
	int16_t* s = malloc((size_t)frames * sizeof(int16_t));
	unsigned int state = 12345u;

	for(int i = 0; i < frames; i++) {
		state ^= state << 13;
		state ^= state >> 17;
		state ^= state << 5;
		s[i] = (int16_t)((int)(state & 0xFFFF) - 32768) / 2;
	}

	return s;
}

// Render a number of frames and say how loud they were, and how jagged
static float render_rms(int frames, float* jagged) {
	int16_t block[BLOCK * 2];
	double sum = 0.0;
	double steps = 0.0;
	int count = 0;
	float previous = 0.0f;

	for(int done = 0; done < frames; done += BLOCK) {
		mixer_render(block, BLOCK);
		for(int i = 0; i < BLOCK; i++) {
			float v = (float)block[i * 2] / 32768.0f;

			assert(block[i * 2] == block[i * 2 + 1]);
			sum += (double)v * (double)v;
			steps += fabs((double)v - (double)previous);
			previous = v;
			count++;
		}
	}
	if(jagged)
		*jagged = (float)(steps / (double)count);

	return (float)sqrt(sum / (double)count);
}

static void test_nothing_playing_is_silent(void) {
	mixer_init();
	assert(render_rms(MIXER_RATE, NULL) == 0.0f);
	mixer_shutdown();
}

static void test_a_bed_is_silent_until_given_a_gain_and_then_eases_in(void) {
	int16_t* noise = make_noise(MIXER_RATE);
	int bed;
	float full;
	float half;

	mixer_init();
	bed = mixer_loop_add(noise, MIXER_RATE);
	assert(bed == 0);
	assert(render_rms(MIXER_RATE, NULL) == 0.0f);
	mixer_loop_set(bed, 1.0f, 0.0f);
	render_rms(MIXER_RATE, NULL);
	full = render_rms(MIXER_RATE, NULL);
	assert(full > 0.15f);
	mixer_loop_set(bed, 0.5f, 0.0f);
	render_rms(MIXER_RATE, NULL);
	half = render_rms(MIXER_RATE, NULL);
	assert(half > full * 0.45f && half < full * 0.55f);
	mixer_shutdown();
	free(noise);
}

static void test_a_gain_change_has_no_jump(void) {
	int16_t* noise = make_noise(MIXER_RATE);
	int16_t block[BLOCK * 2];
	float last = 0.0f;
	int bed;

	mixer_init();
	bed = mixer_loop_add(noise, MIXER_RATE);
	mixer_loop_set(bed, 1.0f, 0.0f);
	// The first block after the change is not already at full gain
	mixer_render(block, BLOCK);
	for(int i = 0; i < BLOCK; i++)
		last = fmaxf(last, fabsf((float)block[i * 2] / 32768.0f));
	assert(last < 0.2f);
	mixer_shutdown();
	free(noise);
}

static void test_muffling_takes_the_highs_out(void) {
	int16_t* noise = make_noise(MIXER_RATE);
	float bright;
	float dull;
	int bed;

	mixer_init();
	bed = mixer_loop_add(noise, MIXER_RATE);
	mixer_loop_set(bed, 1.0f, 0.0f);
	render_rms(MIXER_RATE, NULL);
	render_rms(MIXER_RATE, &bright);
	mixer_loop_set(bed, 1.0f, 1.0f);
	render_rms(MIXER_RATE, NULL);
	render_rms(MIXER_RATE, &dull);
	assert(dull < bright * 0.5f);
	mixer_shutdown();
	free(noise);
}

static void test_a_one_shot_plays_once_and_ends(void) {
	int frames = MIXER_RATE / 2;
	int16_t* noise = make_noise(frames);

	mixer_init();
	mixer_play(noise, frames, 1.0f, 0.0f);
	assert(render_rms(frames - BLOCK, NULL) > 0.1f);
	// Past its end there is nothing left
	render_rms(BLOCK * 2, NULL);
	assert(render_rms(MIXER_RATE, NULL) == 0.0f);
	mixer_shutdown();
	free(noise);
}

static void test_a_full_house_drops_the_extra_sound(void) {
	int frames = MIXER_RATE;
	int16_t* noise = make_noise(frames);

	mixer_init();
	for(int i = 0; i < MIXER_MAX_SHOTS + 3; i++)
		mixer_play(noise, frames, 0.05f, 0.0f);
	assert(render_rms(BLOCK, NULL) > 0.0f);
	mixer_shutdown();
	free(noise);
}

static void test_silence_fades_the_beds_and_cuts_the_shots(void) {
	int16_t* noise = make_noise(MIXER_RATE);
	int bed;

	mixer_init();
	bed = mixer_loop_add(noise, MIXER_RATE);
	mixer_loop_set(bed, 1.0f, 0.0f);
	mixer_play(noise, MIXER_RATE, 1.0f, 0.0f);
	render_rms(MIXER_RATE, NULL);
	mixer_silence();
	render_rms(MIXER_RATE, NULL);
	assert(render_rms(MIXER_RATE, NULL) == 0.0f);
	mixer_shutdown();
	free(noise);
}

static void test_loud_sounds_together_do_not_clip_hard(void) {
	int16_t* loud = malloc(MIXER_RATE * sizeof(int16_t));
	int16_t block[BLOCK * 2];

	for(int i = 0; i < MIXER_RATE; i++)
		loud[i] = i % 2 ? 32000 : -32000;
	mixer_init();
	for(int i = 0; i < MIXER_MAX_SHOTS; i++)
		mixer_play(loud, MIXER_RATE, 1.0f, 0.0f);
	mixer_render(block, BLOCK);
	for(int i = 0; i < BLOCK * 2; i++)
		assert(block[i] > -32768 && block[i] < 32767);
	mixer_shutdown();
	free(loud);
}

int main(void) {
	test_nothing_playing_is_silent();
	test_a_bed_is_silent_until_given_a_gain_and_then_eases_in();
	test_a_gain_change_has_no_jump();
	test_muffling_takes_the_highs_out();
	test_a_one_shot_plays_once_and_ends();
	test_a_full_house_drops_the_extra_sound();
	test_silence_fades_the_beds_and_cuts_the_shots();
	test_loud_sounds_together_do_not_clip_hard();
	printf("mixer_test: all passed\n");

	return 0;
}
