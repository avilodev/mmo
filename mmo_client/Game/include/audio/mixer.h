#ifndef MIXER_H
#define MIXER_H

// A small software mixer: a few looping beds and one-shot sounds added
// together and played on one output. The weather uses it, since PlaySound can
// only play one sound at a time. Sounds are mono 16-bit at MIXER_RATE and are
// not copied: the caller keeps them alive until the mixer is shut down.

#include <stdint.h>

#define MIXER_RATE 22050
#define MIXER_MAX_LOOPS 8
#define MIXER_MAX_SHOTS 8

// Start the mixer and open the output. Returns nonzero when sound can play; on a
// computer with no output, or no backend, the mixer still runs, silently.
int mixer_init(void);

void mixer_shutdown(void);

// Add a looping bed, silent until given a gain. Returns its voice, or -1.
int mixer_loop_add(const int16_t* samples, int frames);

// Set how loud a bed plays and how muffled, each 0 to 1. Changes ease in over
// a moment, so there are no clicks.
void mixer_loop_set(int voice, float gain, float muffle);

// Play a sound once, now, at a gain and a muffling. Dropped when all voices are busy.
void mixer_play(const int16_t* samples, int frames, float gain, float muffle);

// Fade every bed out and stop every one-shot
void mixer_silence(void);

// Mix frames of stereo 16-bit into out. The output thread calls this; so do the tests.
void mixer_render(int16_t* out, int frames);

#endif /* MIXER_H */
