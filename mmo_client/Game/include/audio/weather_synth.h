#ifndef WEATHER_SYNTH_H
#define WEATHER_SYNTH_H

// The sounds of the weather, made from noise in code, so the game needs no
// sound files for them. Mono, 16-bit, WEATHER_SYNTH_RATE samples a second.
// Every sound is the same each time it is made for the same seed.

#include <stdint.h>

#define WEATHER_SYNTH_RATE 22050

// How hard a rain bed falls
typedef enum {
	RAIN_LIGHT, // a few big drops on a soft hiss
	RAIN_MODERATE, // steady rain
	RAIN_HEAVY // a roar, with the patter packed close
} weather_rain_kind;

// A bed of rain that loops without a seam: noise with fine patter in it
void weather_synth_rain_kind(int16_t* out, int frames, weather_rain_kind kind);

// Moderate rain, as it was before there were kinds
void weather_synth_rain(int16_t* out, int frames);

// Wind that rises and falls and loops without a seam: a soft breeze, or a gale's howl
void weather_synth_wind_gale(int16_t* out, int frames, int gale);

// The soft breeze
void weather_synth_wind(int16_t* out, int frames);

// One clap of thunder: a crack, then a rumble that rolls and dies away. The
// seed picks which one: 0 is the closest and sharpest, 2 is far and soft.
void weather_synth_thunder(int16_t* out, int frames, unsigned int seed);

#endif /* WEATHER_SYNTH_H */
