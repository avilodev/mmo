#ifndef AUDIO_H
#define AUDIO_H

// ============================================================================
// AUDIO SYSTEM
// Windows winmm backend.  Volumes are 0.0 - 1.0.
// SFX:   WAV files played asynchronously via PlaySound.
// Music: any format supported by MCI (WAV / MP3 / MIDI) via mciSendString.
// Files that don't exist are silently skipped.
// ============================================================================

void audio_init(void);
void audio_cleanup(void);

// Volume control (call after loading settings)
void audio_set_master_volume(float v);  // Affects both SFX and music
void audio_set_music_volume(float v);
void audio_set_sfx_volume(float v);

// Sound effects — async, fire-and-forget
// path: relative path to .wav, e.g. "Game/Sounds/hit.wav"
void audio_play_sfx(const char* path);

// Background music — loops by default
// path: relative path to .wav/.mp3/.mid
void audio_play_music(const char* path, int loop);
void audio_stop_music(void);

// Named sound event helpers (no-ops until assets are placed)
void audio_event_ability_cast(void);
void audio_event_hit(void);
void audio_event_death(void);
void audio_event_level_up(void);
void audio_event_pickup(void);
void audio_event_ui_click(void);

#endif // AUDIO_H
