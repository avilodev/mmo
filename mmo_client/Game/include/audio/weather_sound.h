#ifndef WEATHER_SOUND_H
#define WEATHER_SOUND_H

// The sound of the weather: rain on the roofs, wind, and thunder that follows
// each flash of lightning. It reads weather_now_get() and plays through the
// mixer, with the sounds made in code (weather_synth.h). Quieter and duller
// when the player is under cover.

// Make the sounds and give the mixer its beds. Call after mixer_init().
void weather_sound_init(void);

void weather_sound_shutdown(void);

// How loud each part of the weather is, 0 to 1 each: the weather as a whole,
// then the rain, the wind and the thunder within it. Takes effect at once.
void weather_sound_set_volumes(float weather, float rain, float wind, float thunder);

// Follow the weather for a frame. covered is 1 when the player stands under a roof
// and 0 in the open.
void weather_sound_update(float dt, int covered);

// Fade everything out, as when the player leaves the world
void weather_sound_silence(void);

#endif /* WEATHER_SOUND_H */
