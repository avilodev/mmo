#ifndef AUDIO_H
#define AUDIO_H

/**
 * @file
 * Declare the Windows multimedia audio interface.
 * Volume values range from 0.0 to 1.0; missing files are ignored.
 */

void audio_init(void);
void audio_cleanup(void);

void audio_set_master_volume(float v);
void audio_set_music_volume(float v);
void audio_set_sfx_volume(float v);

/** Play a WAV effect asynchronously from a relative path. */
void audio_play_sfx(const char* path);

/** Play WAV, MP3, or MIDI music from a relative path. */
void audio_play_music(const char* path, int loop);
void audio_stop_music(void);

void audio_event_ability_cast(void);
void audio_event_hit(void);
void audio_event_death(void);
void audio_event_level_up(void);
void audio_event_pickup(void);
void audio_event_ui_click(void);

#endif // AUDIO_H
