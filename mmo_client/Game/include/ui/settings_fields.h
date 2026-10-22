#ifndef SETTINGS_FIELDS_H
#define SETTINGS_FIELDS_H

/**
 * @file
 * Declare the settings the menu lists, and how each reads and writes a GameSettings.
 *
 * No drawing and no input: the settings menu walks this table, so a new setting
 * is one row here and nothing else in the menu.
 */

#include "core/game_types.h"
#include <stddef.h>

typedef enum {
	SF_TAB_GAMEPLAY,
	SF_TAB_KEYBINDS,
	SF_TAB_VIDEO,
	SF_TAB_AUDIO,
	SF_TAB_INTERFACE,
	SF_TAB_COUNT,
} sf_tab;

typedef enum {
	SF_MASTER_VOLUME,
	SF_MUSIC_VOLUME,
	SF_SFX_VOLUME,
	SF_SHOW_FPS,
	SF_WINDOW_MODE,
	SF_VSYNC,
	SF_FPS_LIMIT,
	SF_UI_SCALE,
	SF_SPRINT,
	SF_RESOLUTION,
	SF_CLOCK_FORMAT,
	SF_TEMP_UNIT,
	SF_SHOW_ANIMAL,
	SF_WEATHER_VOLUME,
	SF_RAIN_VOLUME,
	SF_WIND_VOLUME,
	SF_THUNDER_VOLUME,
	SF_COUNT,
} sf_field;

typedef enum {
	SF_SLIDER,
	SF_TOGGLE,
	SF_CHOICE,
} sf_kind;

typedef struct {
	sf_field field;
	sf_tab tab;
	sf_kind kind;
	const char* label;
	float lo;      // slider range
	float hi;
	float step;    // what an arrow button moves a slider by
} sf_row;

const char* sf_tab_name(sf_tab tab);

// The rows of one tab, in the order the menu lists them
int sf_tab_row_count(sf_tab tab);
const sf_row* sf_tab_row(sf_tab tab, int n);

// A slider or toggle value; a toggle is 0 or 1
float sf_get(const GameSettings* s, sf_field field);
void sf_set(GameSettings* s, sf_field field, float value);

// A choice is one of a short list of named values
int sf_choice_count(sf_field field);
const char* sf_choice_label(sf_field field, int index);
int sf_choice_get(const GameSettings* s, sf_field field);
void sf_choice_set(GameSettings* s, sf_field field, int index);

// Tell the table the screen's size, so the resolution list can end with it and
// leave off the sizes above it; 0 by 0 means not known yet
void sf_set_screen_size(int w, int h);

// Window size in pixels of a resolution table entry; 0 by 0 for entry 0, the screen itself
void sf_resolution_size(int index, int* w, int* h);

// Whether a resolution table entry is no bigger than the screen

int sf_resolution_fits(int index, int screen_w, int screen_h);

// A row that shows but cannot be changed: Resolution while Fullscreen is chosen
int sf_row_locked(const GameSettings* s, sf_field field);

// The value as the menu shows it next to a slider
void sf_value_text(const GameSettings* s, sf_field field, char* out, size_t size);

void sf_defaults(GameSettings* s);

// Put the settings of one tab back to their defaults, leaving the rest
void sf_reset_tab(GameSettings* s, sf_tab tab);

int sf_equal(const GameSettings* a, const GameSettings* b);

#endif // SETTINGS_FIELDS_H
