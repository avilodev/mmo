/**
 * @file
 * Check the settings table the menu is built from.
 */
#include "ui/settings_fields.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static void test_every_field_is_listed_once(void) {
	int seen[SF_COUNT] = { 0 };

	for(int tab = 0; tab < SF_TAB_COUNT; tab++) {
		for(int n = 0; n < sf_tab_row_count((sf_tab)tab); n++) {
			const sf_row* row = sf_tab_row((sf_tab)tab, n);

			assert(row);
			assert(row->tab == (sf_tab)tab);
			seen[row->field]++;
		}

		assert(!sf_tab_row((sf_tab)tab, sf_tab_row_count((sf_tab)tab)));
	}

	for(int f = 0; f < SF_COUNT; f++)
		assert(seen[f] == 1);

	// Keybinds draws its own page, so it lists no rows
	assert(sf_tab_row_count(SF_TAB_KEYBINDS) == 0);
	assert(strcmp(sf_tab_name(SF_TAB_AUDIO), "Audio") == 0);
}

static void test_sliders_clamp(void) {
	GameSettings s;

	sf_defaults(&s);
	sf_set(&s, SF_MASTER_VOLUME, 4.0f);
	assert(s.master_volume == 1.0f);
	sf_set(&s, SF_MASTER_VOLUME, -1.0f);
	assert(s.master_volume == 0.0f);
	sf_set(&s, SF_UI_SCALE, 9.0f);
	assert(s.ui_scale == 1.5f);
	sf_set(&s, SF_UI_SCALE, 0.1f);
	assert(s.ui_scale == 0.75f);
}

static void test_toggles(void) {
	GameSettings s;

	sf_defaults(&s);
	sf_set(&s, SF_SHOW_FPS, 1.0f);
	assert(s.show_fps == 1 && sf_get(&s, SF_SHOW_FPS) == 1.0f);
	sf_set(&s, SF_SHOW_FPS, 0.0f);
	assert(s.show_fps == 0);

	// animal features are on until turned off, and are listed on the gameplay tab
	assert(s.show_animal_features == 1 && sf_get(&s, SF_SHOW_ANIMAL) == 1.0f);
	sf_set(&s, SF_SHOW_ANIMAL, 0.0f);
	assert(s.show_animal_features == 0);
	{
		GameSettings other;

		sf_defaults(&other);
		assert(!sf_equal(&s, &other));
		sf_reset_tab(&s, SF_TAB_GAMEPLAY);
		assert(s.show_animal_features == 1);
	}
}

static void test_choices(void) {
	GameSettings s;
	char text[16];

	sf_defaults(&s);
	assert(sf_choice_count(SF_SPRINT) == 2);
	assert(strcmp(sf_choice_label(SF_SPRINT, sf_choice_get(&s, SF_SPRINT)), "Hold") == 0);
	sf_choice_set(&s, SF_SPRINT, 1);
	assert(s.sprint_toggle == 1);

	sf_choice_set(&s, SF_FPS_LIMIT, 0);
	assert(s.fps_limit == 0);
	assert(strcmp(sf_choice_label(SF_FPS_LIMIT, sf_choice_get(&s, SF_FPS_LIMIT)), "Unlimited") == 0);
	sf_choice_set(&s, SF_FPS_LIMIT, 3);
	assert(s.fps_limit == 120);

	// A cap edited by hand shows as the nearest listed one
	s.fps_limit = 100;
	assert(sf_choice_get(&s, SF_FPS_LIMIT) == 3);

	// Out of range changes nothing
	sf_choice_set(&s, SF_FPS_LIMIT, 99);
	assert(s.fps_limit == 100);

	sf_value_text(&s, SF_UI_SCALE, text, sizeof(text));
	assert(strcmp(text, "1.00x") == 0);
	sf_set(&s, SF_MUSIC_VOLUME, 0.5f);
	sf_value_text(&s, SF_MUSIC_VOLUME, text, sizeof(text));
	assert(strcmp(text, "50") == 0);
}

static void test_clock_format(void) {
	GameSettings s;

	sf_defaults(&s);

	// Starts as the 12-hour clock
	assert(s.clock_24h == 0);
	assert(sf_choice_count(SF_CLOCK_FORMAT) == 2);
	assert(strcmp(sf_choice_label(SF_CLOCK_FORMAT, 0), "12-hour") == 0);
	assert(strcmp(sf_choice_label(SF_CLOCK_FORMAT, 1), "24-hour") == 0);
	assert(sf_choice_get(&s, SF_CLOCK_FORMAT) == 0);

	sf_choice_set(&s, SF_CLOCK_FORMAT, 1);
	assert(s.clock_24h == 1 && sf_choice_get(&s, SF_CLOCK_FORMAT) == 1);

	// Out of range changes nothing
	sf_choice_set(&s, SF_CLOCK_FORMAT, 2);
	assert(s.clock_24h == 1);

	// It is a row of the Interface tab, and that tab's reset puts it back
	sf_reset_tab(&s, SF_TAB_INTERFACE);
	assert(s.clock_24h == 0);
}

static void test_temperature_unit(void) {
	GameSettings s;

	sf_defaults(&s);

	// Starts as Fahrenheit
	assert(s.temp_celsius == 0);
	assert(sf_choice_count(SF_TEMP_UNIT) == 2);
	assert(strcmp(sf_choice_label(SF_TEMP_UNIT, 0), "Fahrenheit") == 0);
	assert(strcmp(sf_choice_label(SF_TEMP_UNIT, 1), "Celsius") == 0);
	assert(sf_choice_get(&s, SF_TEMP_UNIT) == 0);

	sf_choice_set(&s, SF_TEMP_UNIT, 1);
	assert(s.temp_celsius == 1 && sf_choice_get(&s, SF_TEMP_UNIT) == 1);

	// Out of range changes nothing
	sf_choice_set(&s, SF_TEMP_UNIT, 2);
	assert(s.temp_celsius == 1);

	// It is a row of the Interface tab, and that tab's reset puts it back
	sf_reset_tab(&s, SF_TAB_INTERFACE);
	assert(s.temp_celsius == 0);
}

static void test_window_mode(void) {
	GameSettings s;

	sf_defaults(&s);

	// Starts as a borderless window, the way the game has always opened
	assert(s.window_mode == WINDOW_MODE_BORDERLESS);
	assert(sf_choice_count(SF_WINDOW_MODE) == 3);
	assert(strcmp(sf_choice_label(SF_WINDOW_MODE, 0), "Windowed") == 0);
	assert(strcmp(sf_choice_label(SF_WINDOW_MODE, 1), "Borderless Windowed") == 0);
	assert(strcmp(sf_choice_label(SF_WINDOW_MODE, 2), "Fullscreen") == 0);
	assert(sf_choice_get(&s, SF_WINDOW_MODE) == WINDOW_MODE_BORDERLESS);

	sf_choice_set(&s, SF_WINDOW_MODE, WINDOW_MODE_FULLSCREEN);
	assert(s.window_mode == WINDOW_MODE_FULLSCREEN);
	sf_choice_set(&s, SF_WINDOW_MODE, 3);
	assert(s.window_mode == WINDOW_MODE_FULLSCREEN);
	sf_choice_set(&s, SF_WINDOW_MODE, WINDOW_MODE_WINDOWED);
	assert(s.window_mode == WINDOW_MODE_WINDOWED);
}

static void test_resolution_sizes(void) {
	int w = -1;
	int h = -1;
	static const int sizes[][2] = {
		{1280, 720}, {1600, 900}, {1920, 1080}, {2560, 1440}, {3840, 2160}
	};

	for(int i = 1; i <= 5; i++) {
		sf_resolution_size(i, &w, &h);
		assert(w == sizes[i - 1][0] && h == sizes[i - 1][1]);
		assert(w * 9 == h * 16);
	}

	// Entry 0 is the screen itself and has no size of its own
	sf_resolution_size(0, &w, &h);
	assert(w == 0 && h == 0);
}

static void test_resolution_list_follows_the_screen(void) {
	GameSettings s;

	sf_defaults(&s);

	// Screen not known yet: all five, then the screen
	sf_set_screen_size(0, 0);
	assert(s.resolution == 0);
	assert(sf_choice_count(SF_RESOLUTION) == 6);
	assert(strcmp(sf_choice_label(SF_RESOLUTION, 0), "1280 x 720") == 0);

	// 1440p screen: sizes up to it, ending with its own size, no "native" wording
	sf_set_screen_size(2560, 1440);
	assert(sf_choice_count(SF_RESOLUTION) == 4);
	assert(strcmp(sf_choice_label(SF_RESOLUTION, 0), "1280 x 720") == 0);
	assert(strcmp(sf_choice_label(SF_RESOLUTION, 2), "1920 x 1080") == 0);
	assert(strcmp(sf_choice_label(SF_RESOLUTION, 3), "2560 x 1440") == 0);
	assert(strcmp(sf_choice_label(SF_RESOLUTION, 4), "") == 0);

	// First launch lands on the screen's own size
	assert(sf_choice_get(&s, SF_RESOLUTION) == 3);

	// 1080p screen
	sf_set_screen_size(1920, 1080);
	assert(sf_choice_count(SF_RESOLUTION) == 3);
	assert(strcmp(sf_choice_label(SF_RESOLUTION, 2), "1920 x 1080") == 0);
	assert(sf_choice_get(&s, SF_RESOLUTION) == 2);

	// 4K screen
	sf_set_screen_size(3840, 2160);
	assert(sf_choice_count(SF_RESOLUTION) == 5);
	assert(strcmp(sf_choice_label(SF_RESOLUTION, 4), "3840 x 2160") == 0);

	// A laptop panel that is not a standard size gets its own entry last
	sf_set_screen_size(1366, 768);
	assert(sf_choice_count(SF_RESOLUTION) == 2);
	assert(strcmp(sf_choice_label(SF_RESOLUTION, 0), "1280 x 720") == 0);
	assert(strcmp(sf_choice_label(SF_RESOLUTION, 1), "1366 x 768") == 0);

	sf_set_screen_size(0, 0);
}

static void test_resolution_choice_maps_to_the_table(void) {
	GameSettings s;

	sf_defaults(&s);
	sf_set_screen_size(2560, 1440);

	// A position picks that size, the last position follows the screen
	sf_choice_set(&s, SF_RESOLUTION, 1);
	assert(s.resolution == 2 && sf_choice_get(&s, SF_RESOLUTION) == 1);
	sf_choice_set(&s, SF_RESOLUTION, 3);
	assert(s.resolution == 0 && sf_choice_get(&s, SF_RESOLUTION) == 3);
	sf_choice_set(&s, SF_RESOLUTION, 4);
	assert(s.resolution == 0);

	// A size saved for a bigger screen shows as this screen's own size
	s.resolution = 5;
	assert(sf_choice_get(&s, SF_RESOLUTION) == 3);

	sf_set_screen_size(0, 0);
}

static void test_fullscreen_locks_the_resolution(void) {
	GameSettings s;

	sf_defaults(&s);
	sf_set_screen_size(2560, 1440);
	sf_choice_set(&s, SF_RESOLUTION, 1);
	assert(!sf_row_locked(&s, SF_RESOLUTION));

	sf_choice_set(&s, SF_WINDOW_MODE, WINDOW_MODE_FULLSCREEN);
	assert(sf_row_locked(&s, SF_RESOLUTION));
	assert(!sf_row_locked(&s, SF_WINDOW_MODE));
	assert(!sf_row_locked(&s, SF_VSYNC));

	// Shows the screen's size, cannot be changed, and the earlier choice survives
	assert(sf_choice_get(&s, SF_RESOLUTION) == 3);
	sf_choice_set(&s, SF_RESOLUTION, 0);
	assert(s.resolution == 2);

	sf_choice_set(&s, SF_WINDOW_MODE, WINDOW_MODE_WINDOWED);
	assert(!sf_row_locked(&s, SF_RESOLUTION));
	assert(sf_choice_get(&s, SF_RESOLUTION) == 1);

	sf_set_screen_size(0, 0);
}

static void test_resolution_fits_the_screen(void) {
	// Up to the screen's size, either way
	assert(sf_resolution_fits(0, 1920, 1080));
	assert(sf_resolution_fits(2, 1920, 1080));
	assert(sf_resolution_fits(3, 1920, 1080));
	assert(!sf_resolution_fits(4, 1920, 1080));
	assert(sf_resolution_fits(3, 1920, 1200));
	assert(!sf_resolution_fits(5, 2560, 1440));
	assert(sf_resolution_fits(5, 3840, 2160));
}

static void test_reset_tab_leaves_other_tabs(void) {
	GameSettings s;
	GameSettings d;

	sf_defaults(&d);
	sf_defaults(&s);
	sf_set(&s, SF_MASTER_VOLUME, 0.1f);
	sf_set(&s, SF_VSYNC, 0.0f);
	sf_choice_set(&s, SF_FPS_LIMIT, 4);
	sf_choice_set(&s, SF_RESOLUTION, 2);
	sf_choice_set(&s, SF_WINDOW_MODE, WINDOW_MODE_WINDOWED);

	sf_reset_tab(&s, SF_TAB_AUDIO);
	assert(s.master_volume == d.master_volume);
	assert(s.vsync == 0 && s.fps_limit == 144 && s.resolution == 3);
	assert(s.window_mode == WINDOW_MODE_WINDOWED);

	sf_reset_tab(&s, SF_TAB_VIDEO);
	assert(s.vsync == d.vsync && s.fps_limit == d.fps_limit);
	assert(s.resolution == 0 && s.window_mode == WINDOW_MODE_BORDERLESS);
	assert(sf_equal(&s, &d));
}

static void test_equal(void) {
	GameSettings a;
	GameSettings b;

	sf_defaults(&a);
	b = a;
	assert(sf_equal(&a, &b));
	b.sprint_toggle = 1;
	assert(!sf_equal(&a, &b));
	b = a;
	b.resolution = 4;
	assert(!sf_equal(&a, &b));
	b = a;
	b.window_mode = WINDOW_MODE_FULLSCREEN;
	assert(!sf_equal(&a, &b));
	b = a;
	b.clock_24h = 1;
	assert(!sf_equal(&a, &b));
	b = a;
	b.temp_celsius = 1;
	assert(!sf_equal(&a, &b));
	b = a;
	b.sfx_volume += 0.01f;
	assert(!sf_equal(&a, &b));
}

static void test_weather_volumes(void) {
	GameSettings s;
	GameSettings b;
	char text[16];

	sf_defaults(&s);
	// Every weather sound starts at full, within the weather volume
	assert(sf_get(&s, SF_WEATHER_VOLUME) == 1.0f);
	assert(sf_get(&s, SF_RAIN_VOLUME) == 1.0f);
	assert(sf_get(&s, SF_WIND_VOLUME) == 1.0f);
	assert(sf_get(&s, SF_THUNDER_VOLUME) == 1.0f);
	sf_set(&s, SF_RAIN_VOLUME, 4.0f);
	assert(s.rain_volume == 1.0f);
	sf_set(&s, SF_WIND_VOLUME, -2.0f);
	assert(s.wind_volume == 0.0f);
	sf_set(&s, SF_THUNDER_VOLUME, 0.4f);
	assert(fabsf(s.thunder_volume - 0.4f) < 0.0001f);
	sf_set(&s, SF_WEATHER_VOLUME, 0.25f);
	assert(fabsf(s.weather_volume - 0.25f) < 0.0001f);
	// They read as 0 to 100 in the menu
	sf_value_text(&s, SF_THUNDER_VOLUME, text, sizeof(text));
	assert(strcmp(text, "40") == 0);
	sf_value_text(&s, SF_WEATHER_VOLUME, text, sizeof(text));
	assert(strcmp(text, "25") == 0);
	// They are listed in the Audio tab, and a reset of that tab puts them back
	sf_reset_tab(&s, SF_TAB_AUDIO);
	assert(s.weather_volume == 1.0f && s.rain_volume == 1.0f);
	assert(s.wind_volume == 1.0f && s.thunder_volume == 1.0f);
	// A change to any of them is a change to the settings
	b = s;
	b.weather_volume = 0.5f;
	assert(!sf_equal(&s, &b));
	b = s;
	b.rain_volume = 0.5f;
	assert(!sf_equal(&s, &b));
	b = s;
	b.wind_volume = 0.5f;
	assert(!sf_equal(&s, &b));
	b = s;
	b.thunder_volume = 0.5f;
	assert(!sf_equal(&s, &b));
}

int main(void) {
	test_weather_volumes();
	test_every_field_is_listed_once();
	test_sliders_clamp();
	test_toggles();
	test_choices();
	test_clock_format();
	test_temperature_unit();
	test_window_mode();
	test_resolution_sizes();
	test_resolution_list_follows_the_screen();
	test_resolution_choice_maps_to_the_table();
	test_fullscreen_locks_the_resolution();
	test_resolution_fits_the_screen();
	test_reset_tab_leaves_other_tabs();
	test_equal();
	printf("settings_fields_test: OK\n");

	return 0;
}
