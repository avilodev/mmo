/**
 * @file
 * Tabulate the settings the menu lists and map each onto a GameSettings.
 */
#include "ui/settings_fields.h"
#include <math.h>
#include <stdlib.h>
#include <stdio.h>

#define UI_SCALE_MIN 0.75f
#define UI_SCALE_MAX 1.5f

// The frame caps the Frame cap dropdown offers; 0 is uncapped
static const int FPS_LIMITS[] = { 0, 30, 60, 120, 144, 240 };
static const char* const FPS_LIMIT_LABELS[] = {
	"Unlimited", "30 FPS", "60 FPS", "120 FPS", "144 FPS", "240 FPS"
};

#define FPS_LIMIT_COUNT ((int)(sizeof(FPS_LIMITS) / sizeof(FPS_LIMITS[0])))

// Entry 0 is the screen's own size; the rest are fixed 16:9 sizes
static const int RESOLUTIONS[][2] = {
	{ 0, 0 }, { 1280, 720 }, { 1600, 900 }, { 1920, 1080 }, { 2560, 1440 }, { 3840, 2160 }
};
static const char* const RESOLUTION_LABELS[] = {
	"", "1280 x 720", "1600 x 900", "1920 x 1080", "2560 x 1440", "3840 x 2160"
};

static int s_screen_w;
static int s_screen_h;
static char s_screen_label[48] = "Screen size";

#define RESOLUTION_COUNT ((int)(sizeof(RESOLUTIONS) / sizeof(RESOLUTIONS[0])))

static const char* const SPRINT_LABELS[] = { "Hold", "Toggle" };

static const char* const CLOCK_FORMAT_LABELS[] = { "12-hour", "24-hour" };

static const char* const TEMP_UNIT_LABELS[] = { "Fahrenheit", "Celsius" };

static const char* const WINDOW_MODE_LABELS[] = {
	"Windowed", "Borderless Windowed", "Fullscreen"
};

#define WINDOW_MODE_COUNT ((int)(sizeof(WINDOW_MODE_LABELS) / sizeof(WINDOW_MODE_LABELS[0])))

static const sf_row ROWS[] = {
	{ SF_SPRINT,        SF_TAB_GAMEPLAY,  SF_CHOICE, "Sprint",           0.0f, 0.0f, 0.0f },
	{ SF_SHOW_ANIMAL,   SF_TAB_GAMEPLAY,  SF_TOGGLE, "Show kemonomini",  0.0f, 1.0f, 1.0f },
	{ SF_WINDOW_MODE,   SF_TAB_VIDEO,     SF_CHOICE, "Window mode",      0.0f, 0.0f, 0.0f },
	{ SF_RESOLUTION,    SF_TAB_VIDEO,     SF_CHOICE, "Resolution",       0.0f, 0.0f, 0.0f },
	{ SF_VSYNC,         SF_TAB_VIDEO,     SF_TOGGLE, "VSync",            0.0f, 1.0f, 1.0f },
	{ SF_FPS_LIMIT,     SF_TAB_VIDEO,     SF_CHOICE, "Frame cap",        0.0f, 0.0f, 0.0f },
	{ SF_MASTER_VOLUME, SF_TAB_AUDIO,     SF_SLIDER, "Master volume",    0.0f, 1.0f, 0.05f },
	{ SF_MUSIC_VOLUME,  SF_TAB_AUDIO,     SF_SLIDER, "Music volume",     0.0f, 1.0f, 0.05f },
	{ SF_SFX_VOLUME,    SF_TAB_AUDIO,     SF_SLIDER, "SFX volume",       0.0f, 1.0f, 0.05f },
	{ SF_WEATHER_VOLUME, SF_TAB_AUDIO,    SF_SLIDER, "Weather volume",   0.0f, 1.0f, 0.05f },
	{ SF_RAIN_VOLUME,   SF_TAB_AUDIO,     SF_SLIDER, "Rain volume",      0.0f, 1.0f, 0.05f },
	{ SF_WIND_VOLUME,   SF_TAB_AUDIO,     SF_SLIDER, "Wind volume",      0.0f, 1.0f, 0.05f },
	{ SF_THUNDER_VOLUME, SF_TAB_AUDIO,    SF_SLIDER, "Thunder volume",   0.0f, 1.0f, 0.05f },
	{ SF_UI_SCALE,      SF_TAB_INTERFACE, SF_SLIDER, "UI scale",         UI_SCALE_MIN, UI_SCALE_MAX, 0.05f },
	{ SF_SHOW_FPS,      SF_TAB_INTERFACE, SF_TOGGLE, "Show FPS",         0.0f, 1.0f, 1.0f },
	{ SF_CLOCK_FORMAT,  SF_TAB_INTERFACE, SF_CHOICE, "Clock format",     0.0f, 0.0f, 0.0f },
	{ SF_TEMP_UNIT,      SF_TAB_INTERFACE, SF_CHOICE, "Temperature",      0.0f, 0.0f, 0.0f },
};

#define ROW_COUNT ((int)(sizeof(ROWS) / sizeof(ROWS[0])))

static const char* const TAB_NAMES[SF_TAB_COUNT] = {
	"Gameplay", "Keybinds", "Video", "Audio", "Interface"
};

const char* sf_tab_name(sf_tab tab) {
	if(tab < 0 || tab >= SF_TAB_COUNT)
		return "";

	return TAB_NAMES[tab];
}

int sf_tab_row_count(sf_tab tab) {
	int count = 0;

	for(int i = 0; i < ROW_COUNT; i++) {
		if(ROWS[i].tab == tab)
			count++;
	}

	return count;
}

const sf_row* sf_tab_row(sf_tab tab, int n) {
	for(int i = 0; i < ROW_COUNT; i++) {
		if(ROWS[i].tab == tab && n-- == 0)
			return &ROWS[i];
	}

	return NULL;
}

static float clampf(float v, float lo, float hi) {
	if(v < lo)
		return lo;

	return v > hi ? hi : v;
}

float sf_get(const GameSettings* s, sf_field field) {
	switch(field) {
	case SF_MASTER_VOLUME: 
		return s->master_volume;
	case SF_MUSIC_VOLUME:  
		return s->music_volume;
	case SF_SFX_VOLUME:    
		return s->sfx_volume;
	case SF_WEATHER_VOLUME: 
		return s->weather_volume;
	case SF_RAIN_VOLUME:   
		return s->rain_volume;
	case SF_WIND_VOLUME:   
		return s->wind_volume;
	case SF_THUNDER_VOLUME: 
		return s->thunder_volume;
	case SF_SHOW_FPS:      
		return s->show_fps ? 1.0f : 0.0f;
	case SF_VSYNC:         
		return s->vsync ? 1.0f : 0.0f;
	case SF_SHOW_ANIMAL:   
		return s->show_animal_features ? 1.0f : 0.0f;
	case SF_UI_SCALE:      
		return s->ui_scale;
	default:               
		return 0.0f;
	}
}

void sf_set(GameSettings* s, sf_field field, float value) {
	switch(field) {
	case SF_MASTER_VOLUME: 
		s->master_volume = clampf(value, 0.0f, 1.0f); 
		break;
	case SF_MUSIC_VOLUME:  
		s->music_volume = clampf(value, 0.0f, 1.0f); 
		break;
	case SF_SFX_VOLUME:    
		s->sfx_volume = clampf(value, 0.0f, 1.0f); 
		break;
	case SF_WEATHER_VOLUME: 
		s->weather_volume = clampf(value, 0.0f, 1.0f); 
		break;
	case SF_RAIN_VOLUME:   
		s->rain_volume = clampf(value, 0.0f, 1.0f); 
		break;
	case SF_WIND_VOLUME:   
		s->wind_volume = clampf(value, 0.0f, 1.0f); 
		break;
	case SF_THUNDER_VOLUME: 
		s->thunder_volume = clampf(value, 0.0f, 1.0f); 
		break;
	case SF_SHOW_FPS:      
		s->show_fps = value >= 0.5f; 
		break;
	case SF_VSYNC:         
		s->vsync = value >= 0.5f; 
		break;
	case SF_SHOW_ANIMAL:   
		s->show_animal_features = value >= 0.5f; 
		break;
	case SF_UI_SCALE:      
		s->ui_scale = clampf(value, UI_SCALE_MIN, UI_SCALE_MAX); 
		break;
	default:               
		break;
	}
}

void sf_set_screen_size(int w, int h) {
	s_screen_w = w;
	s_screen_h = h;

	if(w > 0 && h > 0)
		snprintf(s_screen_label, sizeof(s_screen_label), "%d x %d", w, h);
	else
		snprintf(s_screen_label, sizeof(s_screen_label), "Screen size");
}

int sf_resolution_fits(int index, int screen_w, int screen_h) {
	int w, h;

	sf_resolution_size(index, &w, &h);

	return w <= screen_w && h <= screen_h;
}

// The table entries the dropdown lists, smallest first and the screen's own size last
static int resolution_list(int out[RESOLUTION_COUNT]) {
	int count = 0;

	for(int i = 1; i < RESOLUTION_COUNT; i++) {
		int w, h;

		sf_resolution_size(i, &w, &h);

		if(s_screen_w > 0 && (!sf_resolution_fits(i, s_screen_w, s_screen_h) ||
		                      (w == s_screen_w && h == s_screen_h)))
			continue;

		out[count++] = i;
	}

	out[count++] = 0;

	return count;
}

int sf_row_locked(const GameSettings* s, sf_field field) {
	return field == SF_RESOLUTION && s->window_mode == WINDOW_MODE_FULLSCREEN;
}

int sf_choice_count(sf_field field) {
	int list[RESOLUTION_COUNT];

	if(field == SF_FPS_LIMIT)
		return FPS_LIMIT_COUNT;

	if(field == SF_RESOLUTION)
		return resolution_list(list);

	if(field == SF_WINDOW_MODE)
		return WINDOW_MODE_COUNT;

	return field == SF_SPRINT || field == SF_CLOCK_FORMAT || field == SF_TEMP_UNIT ? 2 : 0;
}

const char* sf_choice_label(sf_field field, int index) {
	int list[RESOLUTION_COUNT];

	if(index < 0 || index >= sf_choice_count(field))
		return "";

	if(field == SF_FPS_LIMIT)
		return FPS_LIMIT_LABELS[index];

	if(field == SF_WINDOW_MODE)
		return WINDOW_MODE_LABELS[index];

	if(field == SF_RESOLUTION) {
		resolution_list(list);

		return list[index] == 0 ? s_screen_label : RESOLUTION_LABELS[list[index]];
	}

	if(field == SF_CLOCK_FORMAT)
		return CLOCK_FORMAT_LABELS[index];

	if(field == SF_TEMP_UNIT)
		return TEMP_UNIT_LABELS[index];

	return SPRINT_LABELS[index];
}

int sf_choice_get(const GameSettings* s, sf_field field) {
	if(field == SF_SPRINT)
		return s->sprint_toggle ? 1 : 0;

	if(field == SF_CLOCK_FORMAT)
		return s->clock_24h ? 1 : 0;

	if(field == SF_TEMP_UNIT)
		return s->temp_celsius ? 1 : 0;

	if(field == SF_WINDOW_MODE)
		return s->window_mode;

	if(field == SF_RESOLUTION) {
		int list[RESOLUTION_COUNT];
		int count = resolution_list(list);

		// Fullscreen is the screen's size, whatever was chosen before
		if(sf_row_locked(s, field))
			return count - 1;

		// A size saved for another screen that is not listed here shows as this screen's
		for(int i = 0; i < count; i++) {
			if(list[i] == s->resolution)
				return i;
		}

		return count - 1;
	}

	if(field != SF_FPS_LIMIT)
		return 0;

	// A cap saved by hand that is not on the list shows as the nearest one
	int best = 0;

	for(int i = 1; i < FPS_LIMIT_COUNT; i++) {
		if(abs(FPS_LIMITS[i] - s->fps_limit) < abs(FPS_LIMITS[best] - s->fps_limit))
			best = i;
	}

	return s->fps_limit <= 0 ? 0 : best;
}

void sf_choice_set(GameSettings* s, sf_field field, int index) {
	int list[RESOLUTION_COUNT];

	if(index < 0 || index >= sf_choice_count(field) || sf_row_locked(s, field))
		return;

	if(field == SF_SPRINT) {
		s->sprint_toggle = index;
	} else if(field == SF_CLOCK_FORMAT) {
		s->clock_24h = index;
	} else if(field == SF_TEMP_UNIT) {
		s->temp_celsius = index;
	} else if(field == SF_FPS_LIMIT) {
		s->fps_limit = FPS_LIMITS[index];
	} else if(field == SF_WINDOW_MODE) {
		s->window_mode = index;
	} else if(field == SF_RESOLUTION) {
		resolution_list(list);
		s->resolution = list[index];
	}
}

void sf_resolution_size(int index, int* w, int* h) {
	if(index < 0 || index >= RESOLUTION_COUNT)
		index = 0;

	*w = RESOLUTIONS[index][0];
	*h = RESOLUTIONS[index][1];
}

void sf_value_text(const GameSettings* s, sf_field field, char* out, size_t size) {
	if(field == SF_UI_SCALE)
		snprintf(out, size, "%.2fx", s->ui_scale);
	else
		snprintf(out, size, "%d", (int)lroundf(sf_get(s, field) * 100.0f));
}

void sf_defaults(GameSettings* s) {
	s->master_volume = 0.7f;
	s->music_volume = 0.5f;
	s->sfx_volume = 0.8f;
	s->weather_volume = 1.0f;
	s->rain_volume = 1.0f;
	s->wind_volume = 1.0f;
	s->thunder_volume = 1.0f;
	s->show_fps = 0;
	s->window_mode = WINDOW_MODE_BORDERLESS;
	s->ui_scale = 1.0f;
	s->vsync = 1;
	s->fps_limit = DEFAULT_FPS_LIMIT;
	s->sprint_toggle = 0;
	s->resolution = 0;
	s->clock_24h = 0;
	s->temp_celsius = 0;
	s->show_animal_features = 1;
}

void sf_reset_tab(GameSettings* s, sf_tab tab) {
	GameSettings defaults;

	sf_defaults(&defaults);

	for(int i = 0; i < ROW_COUNT; i++) {
		if(ROWS[i].tab != tab)
			continue;

		if(ROWS[i].kind == SF_CHOICE)
			sf_choice_set(s, ROWS[i].field, sf_choice_get(&defaults, ROWS[i].field));
		else
			sf_set(s, ROWS[i].field, sf_get(&defaults, ROWS[i].field));
	}
}

int sf_equal(const GameSettings* a, const GameSettings* b) {
	return fabsf(a->master_volume - b->master_volume) < 0.0001f &&
	       fabsf(a->music_volume - b->music_volume) < 0.0001f &&
	       fabsf(a->sfx_volume - b->sfx_volume) < 0.0001f &&
	       fabsf(a->weather_volume - b->weather_volume) < 0.0001f &&
	       fabsf(a->rain_volume - b->rain_volume) < 0.0001f &&
	       fabsf(a->wind_volume - b->wind_volume) < 0.0001f &&
	       fabsf(a->thunder_volume - b->thunder_volume) < 0.0001f &&
	       fabsf(a->ui_scale - b->ui_scale) < 0.0001f &&
	       a->show_fps == b->show_fps &&
	       a->window_mode == b->window_mode &&
	       a->vsync == b->vsync &&
	       a->fps_limit == b->fps_limit &&
	       a->sprint_toggle == b->sprint_toggle &&
	       a->resolution == b->resolution &&
	       a->clock_24h == b->clock_24h &&
	       a->temp_celsius == b->temp_celsius &&
	       a->show_animal_features == b->show_animal_features;
}
