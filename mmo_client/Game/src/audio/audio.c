/**
 * @file
 * Client sound effects and background music.
 *
 * Two backends behind one interface. The real one is Windows winmm and MCI,
 * which is what the game ships with and what everything below _WIN32 is.
 *
 * The other is silence, for every other platform. It exists because the
 * alternative was that this one file -- including <windows.h> and
 * <mmsystem.h> unconditionally -- made the entire game tree impossible to
 * compile anywhere but MSYS2. That put every rendering, UI and state source
 * beyond the reach of CI and of anyone not developing on Windows, to keep
 * audio working on the one platform that has audio.
 *
 * The silent backend is not a port. It is what lets the other several thousand
 * lines be built and checked; a real Linux backend (SDL_mixer, OpenAL,
 * PulseAudio) would replace it here without touching audio.h.
 */

#include "audio/audio.h"

#include <stdio.h>
#include <string.h>

#include "core/client_log.h"

#ifdef _WIN32

#include <windows.h>
#include <mmsystem.h>

static float g_master = 1.0f;
static float g_music  = 1.0f;
static float g_sfx    = 1.0f;
static int   g_music_open = 0;  // 1 if MCI alias "bgm" is open

// Convert 0.0-1.0 to Windows WORD volume (0-0xFFFF) and pack stereo
static DWORD vol_to_dword(float v) {
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    DWORD w = (DWORD)(v * 0xFFFF);
    return (w << 16) | w;
}

// Push master volume to the default wave output device
static void push_wave_volume(void) {
    waveOutSetVolume(NULL, vol_to_dword(g_master * g_sfx));
}

// Push music volume to the open MCI stream
static void push_music_volume(void) {
    if (!g_music_open) return;
    int vol = (int)(g_master * g_music * 1000.0f);  // MCI range 0-1000
    if (vol < 0)    vol = 0;
    if (vol > 1000) vol = 1000;
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "setaudio bgm volume to %d", vol);
    mciSendStringA(cmd, NULL, 0, NULL);
}

/**
 * Initialize the client audio backend.
 */
void audio_init(void) {
    CLOG_INFO("[AUDIO] Initialized (winmm backend)");
}

/**
 * Stop playback and release audio state.
 */
void audio_cleanup(void) {
    // Halt any asynchronous sound effect first. PlaySoundA was started with
    // SND_ASYNC, so a sound may still be playing on its own thread; stopping it
    // before touching MCI keeps the two subsystems from tearing down at once.
    PlaySoundA(NULL, NULL, 0);

    audio_stop_music();

    // Belt and braces: close every MCI device this process still owns, even one
    // opened under a different alias or left behind by a failed open. Shutdown
    // must not depend on g_music_open being an accurate record.
    mciSendStringA("close all", NULL, 0, NULL);

    CLOG_INFO("[AUDIO] Cleaned up");
}

/**
 * Set the clamped master volume and apply it to active outputs.
 */
void audio_set_master_volume(float v) {
    g_master = (v < 0.0f) ? 0.0f : (v > 1.0f) ? 1.0f : v;
    push_wave_volume();
    push_music_volume();
}

/**
 * Set the clamped music volume and apply it to active playback.
 */
void audio_set_music_volume(float v) {
    g_music = (v < 0.0f) ? 0.0f : (v > 1.0f) ? 1.0f : v;
    push_music_volume();
}

/**
 * Set the clamped sound-effect volume and apply it to wave output.
 */
void audio_set_sfx_volume(float v) {
    g_sfx = (v < 0.0f) ? 0.0f : (v > 1.0f) ? 1.0f : v;
    push_wave_volume();
}

/**
 * Start asynchronous playback of a wave-file sound effect.
 *
 * @param path  Non-empty wave-file path; missing files are ignored by the backend.
 */
void audio_play_sfx(const char* path) {
    if (!path || path[0] == '\0') return;
    // PlaySoundA is async; if file missing it silently fails (SND_NODEFAULT)
    PlaySoundA(path, NULL, SND_FILENAME | SND_ASYNC | SND_NODEFAULT);
}

/**
 * Replace current background music with a file-backed MCI stream.
 *
 * @param path  Non-empty path to a format supported by MCI.
 * @param loop  Nonzero to repeat playback.
 */
void audio_play_music(const char* path, int loop) {
    if (!path || path[0] == '\0') return;

    // Close any previously opened stream
    if (g_music_open) {
        mciSendStringA("stop bgm",  NULL, 0, NULL);
        mciSendStringA("close bgm", NULL, 0, NULL);
        g_music_open = 0;
    }

    char cmd[512];
    snprintf(cmd, sizeof(cmd), "open \"%s\" alias bgm", path);
    MCIERROR err = mciSendStringA(cmd, NULL, 0, NULL);
    if (err != 0) {
        CLOG_WARN("[AUDIO] Could not open music: %s (err %lu)", path, (unsigned long)err);
        return;
    }
    g_music_open = 1;
    push_music_volume();

    if (loop) {
        mciSendStringA("play bgm repeat", NULL, 0, NULL);
    } else {
        mciSendStringA("play bgm", NULL, 0, NULL);
    }
    CLOG_INFO("[AUDIO] Playing music: %s", path);
}

/**
 * Stop and close the active background-music stream.
 */
void audio_stop_music(void) {
    if (!g_music_open) return;
    // Clear the flag before the calls, not after: if a close ever wedges, a
    // later audio_cleanup must not queue the same command again behind it.
    g_music_open = 0;
    mciSendStringA("stop bgm",  NULL, 0, NULL);
    mciSendStringA("close bgm", NULL, 0, NULL);
}

/**
 * Play the configured ability-cast sound.
 */
void audio_event_ability_cast(void) { audio_play_sfx("Game/Sounds/ability_cast.wav"); }
/**
 * Play the configured hit sound.
 */
void audio_event_hit(void)          { audio_play_sfx("Game/Sounds/hit.wav"); }
/**
 * Play the configured death sound.
 */
void audio_event_death(void)        { audio_play_sfx("Game/Sounds/death.wav"); }
/**
 * Play the configured level-up sound.
 */
void audio_event_level_up(void)     { audio_play_sfx("Game/Sounds/level_up.wav"); }
/**
 * Play the configured pickup sound.
 */
void audio_event_pickup(void)       { audio_play_sfx("Game/Sounds/pickup.wav"); }
/**
 * Play the configured interface-click sound.
 */
void audio_event_ui_click(void)     { audio_play_sfx("Game/Sounds/ui_click.wav"); }

#else   /* not _WIN32 */

/* The silent backend.
 *
 * Every entry point is present and does nothing audible. Volumes are still
 * stored and clamped, so settings round-trip correctly and the settings panel
 * behaves the same on every platform -- there is simply nothing listening. */

static float g_master = 1.0f;
static float g_music  = 1.0f;
static float g_sfx    = 1.0f;

static float clamp01(float v) {
    return (v < 0.0f) ? 0.0f : (v > 1.0f) ? 1.0f : v;
}

void audio_init(void) {
    CLOG_INFO("[AUDIO] Initialized (silent backend: no audio on this platform)");
}

void audio_cleanup(void) {}

void audio_set_master_volume(float v) { g_master = clamp01(v); }
void audio_set_music_volume(float v)  { g_music  = clamp01(v); }
void audio_set_sfx_volume(float v)    { g_sfx    = clamp01(v); }

void audio_play_sfx(const char* path) { (void)path; }

void audio_play_music(const char* path, int loop) { (void)path; (void)loop; }
void audio_stop_music(void) {}

void audio_event_ability_cast(void) {}
void audio_event_hit(void)          {}
void audio_event_death(void)        {}
void audio_event_level_up(void)     {}
void audio_event_pickup(void)       {}
void audio_event_ui_click(void)     {}

/* Read so the compiler does not warn that the stored volumes go unused; they
 * are kept because a real backend will want them and settings still store
 * them. */
float audio_debug_effective_volume(void);
float audio_debug_effective_volume(void) { return g_master * g_music * g_sfx; }

#endif  /* _WIN32 */
