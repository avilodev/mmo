// Check the forecast the client keeps: days, regions and temperature units

#include "world/weather_state.h"
#include "world_regions.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define DAY_MS 7200000u

// A forecast whose region r, day d is condition d, high 10 * r + d, low -5
static WeatherForecast make_forecast(uint32_t ms_to_next) {
	WeatherForecast f;

	memset(&f, 0, sizeof(f));
	f.day_index = 40;
	f.day_length_ms = DAY_MS;
	f.ms_to_next_day = ms_to_next;
	f.region_count = WORLD_CITY_COUNT;
	for(int r = 0; r < f.region_count; r++)
		for(int d = 0; d < WEATHER_FORECAST_DAYS; d++) {
			f.days[r][d].condition = (uint8_t)(d % WEATHER_CONDITION_COUNT);
			f.days[r][d].temp_hi = (int8_t)(10 * r + d);
			f.days[r][d].temp_lo = -5;
			f.days[r][d].precip_pct = (uint8_t)(10 * d);
		}
	return f;
}

static void test_nothing_is_known_before_the_forecast_arrives(void) {
	WeatherDayData day;

	weather_state_reset();
	assert(!weather_state_synced());
	assert(weather_state_region_count() == 0);
	assert(!weather_state_day(0, 0, 5.0, &day));
	assert(weather_state_ms_to_next_day(5.0) == 0);
	assert(weather_state_days_elapsed(5.0) == 0);
}

static void test_a_forecast_is_kept_and_read_back(void) {
	WeatherForecast f = make_forecast(5000);
	WeatherDayData day;

	weather_state_reset();
	assert(weather_state_apply(&f, 10.0));
	assert(weather_state_synced());
	assert(weather_state_region_count() == WORLD_CITY_COUNT);
	assert(weather_state_day(2, 3, 10.0, &day));
	assert(day.condition == 3 && day.temp_hi == 23 && day.temp_lo == -5 && day.precip_pct == 30);
}

static void test_a_bad_forecast_is_refused_and_the_old_one_kept(void) {
	WeatherForecast good = make_forecast(5000);
	WeatherForecast bad = make_forecast(5000);
	WeatherDayData day;

	weather_state_reset();
	assert(weather_state_apply(&good, 0.0));

	bad.region_count = 0;
	assert(!weather_state_apply(&bad, 1.0));
	bad.region_count = WEATHER_REGION_MAX + 1;
	assert(!weather_state_apply(&bad, 1.0));
	bad = good;
	bad.day_length_ms = 0;
	assert(!weather_state_apply(&bad, 1.0));
	bad = good;
	bad.ms_to_next_day = DAY_MS + 1;
	assert(!weather_state_apply(&bad, 1.0));

	assert(weather_state_synced());
	assert(weather_state_day(0, 0, 0.0, &day));
}

static void test_a_new_weather_day_slides_the_table(void) {
	WeatherForecast f = make_forecast(1000);
	WeatherDayData day;

	weather_state_reset();
	weather_state_apply(&f, 100.0);

	assert(weather_state_days_elapsed(100.5) == 0);
	assert(weather_state_days_elapsed(101.0) == 1);
	assert(weather_state_days_elapsed(101.0 + DAY_MS / 1000.0 - 0.001) == 1);
	assert(weather_state_days_elapsed(101.0 + DAY_MS / 1000.0) == 2);

	// One day on, today is what the packet called tomorrow
	assert(weather_state_day(1, 0, 101.5, &day));
	assert(day.temp_hi == 11);
	assert(weather_state_day(1, 2, 101.5, &day));
	assert(day.temp_hi == 13);
}

static void test_the_table_runs_out_past_its_last_day(void) {
	WeatherForecast f = make_forecast(1000);
	WeatherDayData day;

	weather_state_reset();
	weather_state_apply(&f, 0.0);

	// Today is the packet's day 6 after 6 days, and nothing is known past it
	assert(weather_state_day(0, 0, 1.0 + 5 * (DAY_MS / 1000.0), &day));
	assert(day.temp_hi == 6);
	assert(!weather_state_day(0, 1, 1.0 + 5 * (DAY_MS / 1000.0), &day));
	assert(!weather_state_day(0, 0, 1.0 + 6 * (DAY_MS / 1000.0), &day));
}

static void test_a_day_or_region_out_of_range_is_not_there(void) {
	WeatherForecast f = make_forecast(1000);
	WeatherDayData day;

	weather_state_reset();
	weather_state_apply(&f, 0.0);
	assert(!weather_state_day(-1, 0, 0.0, &day));
	assert(!weather_state_day(WORLD_CITY_COUNT, 0, 0.0, &day));
	assert(!weather_state_day(0, -1, 0.0, &day));
	assert(!weather_state_day(0, WEATHER_FORECAST_DAYS, 0.0, &day));
}

static void test_the_countdown_to_the_next_day(void) {
	WeatherForecast f = make_forecast(5000);

	weather_state_reset();
	weather_state_apply(&f, 10.0);
	assert(weather_state_ms_to_next_day(10.0) == 5000);
	assert(weather_state_ms_to_next_day(12.0) == 3000);
	assert(weather_state_ms_to_next_day(15.0) == DAY_MS);
	assert(weather_state_ms_to_next_day(16.0) == DAY_MS - 1000);
	// A clock that ran backwards is no time at all, not a wrapped one
	assert(weather_state_ms_to_next_day(9.0) == 5000);
}

static void test_the_frame_clock_drives_the_now_readers(void) {
	WeatherForecast f = make_forecast(1000);
	WeatherDayData day;

	weather_state_reset();
	weather_state_apply(&f, 50.0);

	weather_state_tick(50.5);
	assert(weather_state_day_now(0, 0, &day) && day.temp_hi == 0);
	assert(weather_state_ms_to_next_day_now() == 500);

	weather_state_tick(51.5);
	assert(weather_state_day_now(0, 0, &day) && day.temp_hi == 1);
	assert(weather_state_ms_to_next_day_now() == DAY_MS - 500);

	// Forgetting the forecast forgets the frame's time too
	weather_state_reset();
	assert(!weather_state_day_now(0, 0, &day));
	assert(weather_state_ms_to_next_day_now() == 0);
}

static void test_a_reset_forgets_the_forecast(void) {
	WeatherForecast f = make_forecast(5000);
	WeatherDayData day;

	weather_state_apply(&f, 0.0);
	weather_state_reset();
	assert(!weather_state_synced());
	assert(!weather_state_day(0, 0, 0.0, &day));
}

static void test_a_position_is_in_the_region_of_the_nearest_capital(void) {
	for(int i = 0; i < WORLD_CITY_COUNT; i++) {
		float x = (float)WORLD_CITIES[i].tile_x * WORLD_TILE_PX;
		float y = (float)WORLD_CITIES[i].tile_y * WORLD_TILE_PX;

		assert(weather_region_at(x, y) == i);
		assert(weather_region_at(x + 30.0f, y - 30.0f) == i);
	}
}

static void test_region_and_condition_names(void) {
	assert(strcmp(weather_region_name(0), WORLD_CITIES[0].city_name) == 0);
	assert(strcmp(weather_region_name(-1), "") == 0);
	assert(strcmp(weather_region_name(WORLD_CITY_COUNT), "") == 0);
	assert(strcmp(weather_condition_name(WEATHER_CLEAR), "Clear") == 0);
	assert(strcmp(weather_condition_name(WEATHER_PARTLY_CLOUDY), "Partly Cloudy") == 0);
	assert(strcmp(weather_condition_name(WEATHER_THUNDER), "Thunderstorm") == 0);
	assert(strcmp(weather_condition_name(WEATHER_SNOW), "Snow") == 0);
	assert(strcmp(weather_condition_name(99), "") == 0);
}

static void test_day_labels(void) {
	char text[32];

	weather_day_label(0, text, sizeof(text));
	assert(strcmp(text, "Today") == 0);
	weather_day_label(1, text, sizeof(text));
	assert(strcmp(text, "Tomorrow") == 0);
	weather_day_label(4, text, sizeof(text));
	assert(strcmp(text, "In 4 days") == 0);
	weather_day_label(2, text, 4);
	assert(strlen(text) == 3);
}

static void test_the_temperature_swings_through_the_day(void) {
	WeatherDayData day;

	memset(&day, 0, sizeof(day));
	day.temp_hi = 20;
	day.temp_lo = 4;
	assert(weather_temp_at_hour(&day, 15.0f) == 20);
	assert(weather_temp_at_hour(&day, 3.0f) == 4);
	assert(weather_temp_at_hour(&day, 27.0f) == 4);
	assert(weather_temp_at_hour(&day, 9.0f) == 12);
	assert(weather_temp_at_hour(&day, 21.0f) == 12);
	assert(weather_temp_at_hour(&day, 12.0f) > weather_temp_at_hour(&day, 8.0f));
}

static void test_night_is_between_dusk_and_dawn(void) {
	assert(weather_is_night(0.0f));
	assert(weather_is_night(5.99f));
	assert(!weather_is_night(6.0f));
	assert(!weather_is_night(12.0f));
	assert(!weather_is_night(17.99f));
	assert(weather_is_night(18.0f));
	assert(weather_is_night(23.9f));
}

static void test_temperature_units(void) {
	assert(weather_temp_convert(0, 1) == 0);
	assert(weather_temp_convert(0, 0) == 32);
	assert(weather_temp_convert(100, 0) == 212);
	assert(weather_temp_convert(-40, 0) == -40);
	assert(weather_temp_convert(20, 0) == 68);
	assert(weather_temp_convert(22, 0) == 72);
	assert(weather_temp_convert(-3, 0) == 27);
	assert(weather_temp_convert(-4, 0) == 25);
	assert(weather_temp_convert(-100, 0) == -148);
}

static void test_temperature_text(void) {
	char text[16];

	weather_temp_text(14, 1, text, sizeof(text));
	assert(strcmp(text, "14 C") == 0);
	weather_temp_text(14, 0, text, sizeof(text));
	assert(strcmp(text, "57 F") == 0);
	weather_temp_text(-3, 1, text, sizeof(text));
	assert(strcmp(text, "-3 C") == 0);
}

static WeatherDayData day_of(int condition, int intensity, int wind) {
	WeatherDayData d;

	memset(&d, 0, sizeof(d));
	d.condition = (uint8_t)condition;
	d.intensity = (uint8_t)intensity;
	d.wind = (uint8_t)wind;

	return d;
}

static void test_a_day_is_named_for_how_hard_it_falls(void) {
	char name[40];
	WeatherDayData d;

	d = day_of(WEATHER_RAIN, 1, 0);
	weather_day_name(&d, name, sizeof(name));
	assert(strcmp(name, "Light Rain") == 0);
	d = day_of(WEATHER_RAIN, 2, 0);
	weather_day_name(&d, name, sizeof(name));
	assert(strcmp(name, "Rain") == 0);
	d = day_of(WEATHER_RAIN, 3, 0);
	weather_day_name(&d, name, sizeof(name));
	assert(strcmp(name, "Heavy Rain") == 0);
	d = day_of(WEATHER_SNOW, 1, 0);
	weather_day_name(&d, name, sizeof(name));
	assert(strcmp(name, "Light Snow") == 0);
	d = day_of(WEATHER_SNOW, 3, 0);
	weather_day_name(&d, name, sizeof(name));
	assert(strcmp(name, "Heavy Snow") == 0);
	d = day_of(WEATHER_THUNDER, 2, 0);
	weather_day_name(&d, name, sizeof(name));
	assert(strcmp(name, "Thunderstorm") == 0);
	d = day_of(WEATHER_THUNDER, 3, 0);
	weather_day_name(&d, name, sizeof(name));
	assert(strcmp(name, "Severe Thunderstorm") == 0);
	// A dry day has no level to say
	d = day_of(WEATHER_CLOUDY, 3, 0);
	weather_day_name(&d, name, sizeof(name));
	assert(strcmp(name, "Cloudy") == 0);
	// A level a newer server might send is the heaviest known
	d = day_of(WEATHER_RAIN, 9, 0);
	weather_day_name(&d, name, sizeof(name));
	assert(strcmp(name, "Heavy Rain") == 0);
}

static void test_the_short_names_fit_a_row(void) {
	char name[40];
	WeatherDayData d;

	for(int c = 0; c < WEATHER_CONDITION_COUNT; c++)
		for(int level = 0; level <= 3; level++) {
			d = day_of(c, level, 0);
			weather_day_short_name(&d, name, sizeof(name));
			assert(name[0] != '\0' && strlen(name) <= 14);
		}
	d = day_of(WEATHER_THUNDER, 3, 0);
	weather_day_short_name(&d, name, sizeof(name));
	assert(strcmp(name, "Severe Storm") == 0);
}

static void test_the_wind_has_names_and_speeds(void) {
	char text[24];

	assert(strcmp(weather_wind_name(0), "Calm") == 0);
	assert(strcmp(weather_wind_name(1), "Breezy") == 0);
	assert(strcmp(weather_wind_name(2), "Windy") == 0);
	assert(strcmp(weather_wind_name(3), "Gale") == 0);
	assert(strcmp(weather_wind_name(-2), "Calm") == 0);
	assert(strcmp(weather_wind_name(40), "Gale") == 0);
	weather_wind_range_text(1, 1, text, sizeof(text));
	assert(strcmp(text, "10-25 km/h") == 0);
	weather_wind_range_text(1, 0, text, sizeof(text));
	assert(strcmp(text, "6-15 mph") == 0);
	weather_wind_range_text(3, 0, text, sizeof(text));
	assert(strcmp(text, "30+ mph") == 0);
}

static void test_the_day_before_is_kept_when_a_new_day_arrives(void) {
	WeatherForecast first = make_forecast(5000);
	WeatherForecast next = make_forecast(5000);
	WeatherDayData prev;

	weather_state_reset();
	assert(weather_state_apply(&first, 0.0));
	// Nothing came before the first forecast
	assert(!weather_state_prev_day(1, 0.0, &prev));
	// The next day's forecast arrives: yesterday was the first's today
	next.day_index = first.day_index + 1;
	assert(weather_state_apply(&next, 10.0));
	assert(weather_state_prev_day(1, 10.0, &prev));
	assert(prev.condition == first.days[1][0].condition && prev.temp_hi == first.days[1][0].temp_hi);
	// Applying the same day again (a join) keeps it
	assert(weather_state_apply(&next, 11.0));
	assert(weather_state_prev_day(1, 11.0, &prev));
	assert(prev.temp_hi == first.days[1][0].temp_hi);
	// A forecast far in the future has no yesterday we know
	next.day_index = first.day_index + 40;
	assert(weather_state_apply(&next, 12.0));
	assert(!weather_state_prev_day(1, 12.0, &prev));
	// A forecast from the past starts over
	first.day_index = 3;
	assert(weather_state_apply(&first, 13.0));
	assert(!weather_state_prev_day(1, 13.0, &prev));
	assert(!weather_state_prev_day(-1, 13.0, &prev));
}

static void test_a_day_that_slides_in_has_its_yesterday_in_the_table(void) {
	WeatherForecast f = make_forecast(5000);
	WeatherDayData prev;

	weather_state_reset();
	assert(weather_state_apply(&f, 0.0));
	// Five seconds in, the next day has begun, and the table has moved on
	assert(weather_state_days_elapsed(6.0) == 1);
	assert(weather_state_prev_day(2, 6.0, &prev));
	assert(prev.condition == f.days[2][0].condition && prev.temp_hi == f.days[2][0].temp_hi);
	assert(weather_state_days_elapsed(5.0 + 7200.0 + 1.0) == 2);
	assert(weather_state_prev_day(2, 5.0 + 7200.0 + 1.0, &prev));
	assert(prev.temp_hi == f.days[2][1].temp_hi);
}

static void test_how_far_into_the_day_it_is(void) {
	WeatherForecast f = make_forecast(5000);

	weather_state_reset();
	assert(weather_state_into_day_s(3.0) == 0.0);
	assert(weather_state_apply(&f, 100.0));
	// 5 s of the 7200 s day are left: 7195 s have gone
	assert(weather_state_into_day_s(100.0) > 7194.9 && weather_state_into_day_s(100.0) < 7195.1);
	assert(weather_state_into_day_s(102.0) > 7196.9 && weather_state_into_day_s(102.0) < 7197.1);
	// The next day began at 105 s
	assert(weather_state_into_day_s(105.0) < 0.01);
	assert(weather_state_into_day_s(205.0) > 99.9 && weather_state_into_day_s(205.0) < 100.1);
	weather_state_tick(205.0);
	assert(weather_state_into_day_now_s() > 99.9 && weather_state_into_day_now_s() < 100.1);
}

int main(void) {
	test_a_day_is_named_for_how_hard_it_falls();
	test_the_short_names_fit_a_row();
	test_the_wind_has_names_and_speeds();
	test_the_day_before_is_kept_when_a_new_day_arrives();
	test_a_day_that_slides_in_has_its_yesterday_in_the_table();
	test_how_far_into_the_day_it_is();
	test_nothing_is_known_before_the_forecast_arrives();
	test_a_forecast_is_kept_and_read_back();
	test_a_bad_forecast_is_refused_and_the_old_one_kept();
	test_a_new_weather_day_slides_the_table();
	test_the_table_runs_out_past_its_last_day();
	test_a_day_or_region_out_of_range_is_not_there();
	test_the_countdown_to_the_next_day();
	test_the_frame_clock_drives_the_now_readers();
	test_a_reset_forgets_the_forecast();
	test_a_position_is_in_the_region_of_the_nearest_capital();
	test_region_and_condition_names();
	test_day_labels();
	test_the_temperature_swings_through_the_day();
	test_night_is_between_dusk_and_dawn();
	test_temperature_units();
	test_temperature_text();
	printf("weather_state_test: all passed\n");
	return 0;
}
