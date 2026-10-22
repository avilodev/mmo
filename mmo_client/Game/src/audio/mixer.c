// The software mixer and its output.

#include "audio/mixer.h"
#include "core/client_log.h"

#include <math.h>
#include <string.h>

// How fast a gain may change, a second, so a change is never a click
#define GAIN_SLEW 2.0f

// The most frames one mix call does at a time
#define OUT_MAX_BLOCK 4096

// The one-pole filter's coefficient with no muffling, and with all of it
#define MUFFLE_OPEN 1.0f
#define MUFFLE_FULL 0.10f

typedef struct {
	const int16_t* samples;
	int frames;
	int pos;
	int active;
	float gain;
	float gain_target;
	float muffle;
	float muffle_target;
	float filtered;
} voice;

static voice g_loops[MIXER_MAX_LOOPS];
static int g_loop_count;
static voice g_shots[MIXER_MAX_SHOTS];

#ifdef _WIN32

#include <windows.h>
#include <mmsystem.h>

// The output: three buffers of about 46 ms, refilled by a thread
#define OUT_FRAMES 1024
#define OUT_BUFFERS 3

static CRITICAL_SECTION g_lock;
static int g_lock_ready;
static HWAVEOUT g_out;
static WAVEHDR g_headers[OUT_BUFFERS];
static int16_t g_buffers[OUT_BUFFERS][OUT_FRAMES * 2];
static HANDLE g_event;
static HANDLE g_thread;
static volatile int g_running;

static void lock(void) {
	if(g_lock_ready)
		EnterCriticalSection(&g_lock);
}

static void unlock(void) {
	if(g_lock_ready)
		LeaveCriticalSection(&g_lock);
}

#else

static void lock(void) {}
static void unlock(void) {}

#endif

static float clamp01(float v) {
	if(v < 0.0f)
		return 0.0f;

	return v > 1.0f ? 1.0f : v;
}

static float approach(float now, float target, float step) {
	if(now < target)
		return now + step < target ? now + step : target;

	return now - step > target ? now - step : target;
}

// Add a voice's next frames into the mix. Returns 0 when a one-shot ends.
static int add_voice(voice* v, float* mix, int frames, int loops) {
	float step = GAIN_SLEW * (float)frames / (float)MIXER_RATE;
	float from = v->gain;
	float to;

	v->gain = approach(v->gain, v->gain_target, step);
	v->muffle = approach(v->muffle, v->muffle_target, step);
	to = v->gain;
	if(from <= 0.0f && to <= 0.0f && loops) {
		// Silent beds still move on, so they come in where they would have been
		v->pos = (v->pos + frames) % v->frames;

		return 1;
	}
	for(int i = 0; i < frames; i++) {
		float g = from + (to - from) * (float)i / (float)frames;
		float coefficient = MUFFLE_OPEN + (MUFFLE_FULL - MUFFLE_OPEN) * v->muffle;
		float x;

		if(v->pos >= v->frames) {
			if(!loops)
				return 0;
			v->pos = 0;
		}
		x = (float)v->samples[v->pos++] / 32768.0f;
		v->filtered += coefficient * (x - v->filtered);
		mix[i] += v->filtered * g;
	}

	return 1;
}

void mixer_render(int16_t* out, int frames) {
	float mix[OUT_MAX_BLOCK];

	if(frames > OUT_MAX_BLOCK)
		frames = OUT_MAX_BLOCK;
	memset(mix, 0, (size_t)frames * sizeof(float));
	lock();
	for(int i = 0; i < g_loop_count; i++)
		if(g_loops[i].active)
			add_voice(&g_loops[i], mix, frames, 1);
	for(int i = 0; i < MIXER_MAX_SHOTS; i++)
		if(g_shots[i].active && !add_voice(&g_shots[i], mix, frames, 0))
			g_shots[i].active = 0;
	unlock();
	for(int i = 0; i < frames; i++) {
		float v = mix[i];
		// A soft knee so several loud sounds together do not harshly clip
		int16_t s;

		if(v > 0.8f)
			v = 0.8f + (v - 0.8f) / (1.0f + (v - 0.8f) * 5.0f);
		if(v < -0.8f)
			v = -0.8f + (v + 0.8f) / (1.0f - (v + 0.8f) * 5.0f);
		s = (int16_t)lroundf(v * 32767.0f);
		out[i * 2] = s;
		out[i * 2 + 1] = s;
	}
}

int mixer_loop_add(const int16_t* samples, int frames) {
	int voice_index = -1;

	if(!samples || frames <= 0)
		return -1;
	lock();
	if(g_loop_count < MIXER_MAX_LOOPS) {
		voice* v = &g_loops[g_loop_count];

		memset(v, 0, sizeof(*v));
		v->samples = samples;
		v->frames = frames;
		v->active = 1;
		voice_index = g_loop_count++;
	}
	unlock();

	return voice_index;
}

void mixer_loop_set(int voice_index, float gain, float muffle) {
	if(voice_index < 0 || voice_index >= g_loop_count)
		return;
	lock();
	g_loops[voice_index].gain_target = clamp01(gain);
	g_loops[voice_index].muffle_target = clamp01(muffle);
	unlock();
}

void mixer_play(const int16_t* samples, int frames, float gain, float muffle) {
	if(!samples || frames <= 0)
		return;
	lock();
	for(int i = 0; i < MIXER_MAX_SHOTS; i++) {
		voice* v = &g_shots[i];

		if(v->active)
			continue;
		memset(v, 0, sizeof(*v));
		v->samples = samples;
		v->frames = frames;
		v->active = 1;
		// A one-shot plays at its own gain from the first sample
		v->gain = v->gain_target = clamp01(gain);
		v->muffle = v->muffle_target = clamp01(muffle);
		break;
	}
	unlock();
}

void mixer_silence(void) {
	lock();
	for(int i = 0; i < g_loop_count; i++)
		g_loops[i].gain_target = 0.0f;
	for(int i = 0; i < MIXER_MAX_SHOTS; i++)
		g_shots[i].active = 0;
	unlock();
}

#ifdef _WIN32

// Refill each buffer the output has finished with
static DWORD WINAPI output_thread(LPVOID unused) {
	(void)unused;
	while(g_running) {
		WaitForSingleObject(g_event, 100);
		for(int b = 0; b < OUT_BUFFERS && g_running; b++) {
			if(!(g_headers[b].dwFlags & WHDR_DONE))
				continue;
			mixer_render(g_buffers[b], OUT_FRAMES);
			g_headers[b].dwFlags &= ~WHDR_DONE;
			waveOutWrite(g_out, &g_headers[b], sizeof(WAVEHDR));
		}
	}

	return 0;
}

int mixer_init(void) {
	WAVEFORMATEX format;
	MMRESULT result;

	mixer_shutdown();
	InitializeCriticalSection(&g_lock);
	g_lock_ready = 1;
	memset(&format, 0, sizeof(format));
	format.wFormatTag = WAVE_FORMAT_PCM;
	format.nChannels = 2;
	format.nSamplesPerSec = MIXER_RATE;
	format.wBitsPerSample = 16;
	format.nBlockAlign = 4;
	format.nAvgBytesPerSec = MIXER_RATE * 4;
	g_event = CreateEvent(NULL, FALSE, FALSE, NULL);
	result = waveOutOpen(&g_out, WAVE_MAPPER, &format, (DWORD_PTR)g_event, 0, CALLBACK_EVENT);
	if(result != MMSYSERR_NOERROR) {
		CLOG_WARN("[MIXER] No sound output (error %u); the weather will be silent",
		          (unsigned)result);
		g_out = NULL;
		return 0;
	}
	for(int b = 0; b < OUT_BUFFERS; b++) {
		memset(&g_headers[b], 0, sizeof(WAVEHDR));
		g_headers[b].lpData = (LPSTR)g_buffers[b];
		g_headers[b].dwBufferLength = sizeof(g_buffers[b]);
		waveOutPrepareHeader(g_out, &g_headers[b], sizeof(WAVEHDR));
		mixer_render(g_buffers[b], OUT_FRAMES);
		waveOutWrite(g_out, &g_headers[b], sizeof(WAVEHDR));
	}
	g_running = 1;
	g_thread = CreateThread(NULL, 0, output_thread, NULL, 0, NULL);
	CLOG_INFO("[MIXER] Output open at %d Hz", MIXER_RATE);

	return 1;
}

void mixer_shutdown(void) {
	g_running = 0;
	if(g_event)
		SetEvent(g_event);
	if(g_thread) {
		WaitForSingleObject(g_thread, 2000);
		CloseHandle(g_thread);
		g_thread = NULL;
	}
	if(g_out) {
		waveOutReset(g_out);
		for(int b = 0; b < OUT_BUFFERS; b++)
			waveOutUnprepareHeader(g_out, &g_headers[b], sizeof(WAVEHDR));
		waveOutClose(g_out);
		g_out = NULL;
	}
	if(g_event) {
		CloseHandle(g_event);
		g_event = NULL;
	}
	if(g_lock_ready) {
		DeleteCriticalSection(&g_lock);
		g_lock_ready = 0;
	}
	g_loop_count = 0;
	memset(g_loops, 0, sizeof(g_loops));
	memset(g_shots, 0, sizeof(g_shots));
}

#else

int mixer_init(void) {
	g_loop_count = 0;
	memset(g_loops, 0, sizeof(g_loops));
	memset(g_shots, 0, sizeof(g_shots));

	return 0;
}

void mixer_shutdown(void) {
	g_loop_count = 0;
	memset(g_loops, 0, sizeof(g_loops));
	memset(g_shots, 0, sizeof(g_shots));
}

#endif
