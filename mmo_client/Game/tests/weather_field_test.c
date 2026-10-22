// Check how the regions' weather is blended across the map.

#include "world/weather_field.h"
#include "world_regions.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static int near(float a, float b) {
	return fabsf(a - b) < 0.02f;
}

static float city_x(int i) {
	return (float)WORLD_CITIES[i].tile_x * WORLD_TILE_PX;
}

static float city_y(int i) {
	return (float)WORLD_CITIES[i].tile_y * WORLD_TILE_PX;
}

static void fill(WeatherDayData* regions, int condition, int hi, int lo) {
	for(int i = 0; i < WORLD_CITY_COUNT; i++) {
		memset(&regions[i], 0, sizeof(regions[i]));
		regions[i].condition = (uint8_t)condition;
		regions[i].temp_hi = (int8_t)hi;
		regions[i].temp_lo = (int8_t)lo;
	}
}

static void test_a_capital_has_its_own_regions_weather(void) {
	WeatherDayData regions[WEATHER_REGION_MAX];
	weather_sample s;

	fill(regions, WEATHER_CLEAR, 20, 10);
	regions[2].condition = WEATHER_THUNDER;
	regions[2].temp_hi = -4;
	regions[2].temp_lo = -10;

	weather_field_blend(regions, WORLD_CITY_COUNT, city_x(2), city_y(2), &s);
	assert(s.cloud > 0.95f && s.rain > 0.8f && s.storm > 0.95f && s.snow < 0.05f);
	assert(near(s.temp_c, -7.0f));

	weather_field_blend(regions, WORLD_CITY_COUNT, city_x(0), city_y(0), &s);
	assert(s.cloud < 0.1f && s.rain < 0.02f && s.storm < 0.02f);
	assert(near(s.temp_c, 15.0f));
}

static void test_each_condition_means_what_it_says(void) {
	WeatherDayData regions[WEATHER_REGION_MAX];
	weather_sample clear;
	weather_sample partly;
	weather_sample cloudy;
	weather_sample rain;
	weather_sample snow;

	fill(regions, WEATHER_CLEAR, 10, 0);
	weather_field_blend(regions, WORLD_CITY_COUNT, city_x(0), city_y(0), &clear);
	fill(regions, WEATHER_PARTLY_CLOUDY, 10, 0);
	weather_field_blend(regions, WORLD_CITY_COUNT, city_x(0), city_y(0), &partly);
	fill(regions, WEATHER_CLOUDY, 10, 0);
	weather_field_blend(regions, WORLD_CITY_COUNT, city_x(0), city_y(0), &cloudy);
	fill(regions, WEATHER_RAIN, 10, 0);
	weather_field_blend(regions, WORLD_CITY_COUNT, city_x(0), city_y(0), &rain);
	fill(regions, WEATHER_SNOW, -5, -10);
	weather_field_blend(regions, WORLD_CITY_COUNT, city_x(0), city_y(0), &snow);

	assert(clear.cloud < partly.cloud && partly.cloud < cloudy.cloud && cloudy.cloud < rain.cloud);
	assert(rain.rain > 0.5f && cloudy.rain == 0.0f);
	assert(snow.snow > 0.5f && snow.rain == 0.0f && rain.snow == 0.0f);
}

static void test_between_two_capitals_it_is_a_mix(void) {
	WeatherDayData regions[WEATHER_REGION_MAX];
	weather_sample mid;
	float mx = (city_x(0) + city_x(1)) * 0.5f;
	float my = (city_y(0) + city_y(1)) * 0.5f;

	fill(regions, WEATHER_CLEAR, 30, 20);
	regions[0].condition = WEATHER_CLEAR;
	regions[0].temp_hi = 40;
	regions[0].temp_lo = 40;
	regions[1].temp_hi = 0;
	regions[1].temp_lo = 0;

	weather_field_blend(regions, WORLD_CITY_COUNT, mx, my, &mid);

	// Neither region owns the middle
	assert(mid.temp_c > 3.0f && mid.temp_c < 37.0f);
}

static void test_the_blend_is_smooth_not_blocky(void) {
	WeatherDayData regions[WEATHER_REGION_MAX];
	weather_sample a;
	weather_sample b;

	fill(regions, WEATHER_CLEAR, 30, 30);
	regions[1].temp_hi = 0;
	regions[1].temp_lo = 0;

	// A step of one tile never changes the temperature by more than a degree
	for(float f = 0.05f; f < 0.95f; f += 0.05f) {
		float x = city_x(0) + (city_x(1) - city_x(0)) * f;
		float y = city_y(0) + (city_y(1) - city_y(0)) * f;

		weather_field_blend(regions, WORLD_CITY_COUNT, x, y, &a);
		weather_field_blend(regions, WORLD_CITY_COUNT, x + 16.0f, y, &b);
		assert(fabsf(a.temp_c - b.temp_c) < 1.0f);
	}
}

static void test_no_regions_is_calm_not_a_crash(void) {
	weather_sample s;

	weather_field_blend(NULL, 0, 100.0f, 100.0f, &s);
	assert(s.cloud == 0.0f && s.rain == 0.0f && s.storm == 0.0f && s.snow == 0.0f);
}

static void test_noise_is_in_range_and_repeatable(void) {
	for(int i = 0; i < 2000; i++) {
		float x = (float)i * 137.0f;
		float y = (float)i * 91.0f;
		float n = weather_field_noise(x, y, 7u);

		assert(n >= 0.0f && n <= 1.0f);
		assert(n == weather_field_noise(x, y, 7u));
	}
}

static void test_noise_varies_and_follows_the_seed(void) {
	int differ = 0;
	float lo = 1.0f;
	float hi = 0.0f;

	for(int i = 0; i < 400; i++) {
		float x = (float)i * 311.0f;
		float n = weather_field_noise(x, 500.0f, 1u);

		lo = fminf(lo, n);
		hi = fmaxf(hi, n);
		if(n != weather_field_noise(x, 500.0f, 2u))
			differ++;
	}
	assert(hi - lo > 0.5f);
	assert(differ > 300);
}

static void test_noise_has_no_jumps(void) {
	for(int i = 0; i < 400; i++) {
		float x = (float)i * 53.0f;

		assert(fabsf(weather_field_noise(x, 900.0f, 3u) - weather_field_noise(x + 4.0f, 900.0f, 3u)) <
		       0.05f);
	}
}

static void test_roughening_keeps_values_in_range(void) {
	for(int i = 0; i < 500; i++) {
		weather_sample s = { 1.0f, 0.9f, 1.0f, 0.0f, 3.0f };

		weather_field_roughen(&s, (float)i * 200.0f, (float)i * 90.0f, 5u);
		assert(s.cloud >= 0.0f && s.cloud <= 1.0f && s.rain >= 0.0f && s.rain <= 1.0f);
		assert(s.storm >= 0.0f && s.storm <= 1.0f && s.snow == 0.0f && s.temp_c == 3.0f);
	}
}

static void test_a_clear_sky_stays_clear_after_roughening(void) {
	weather_sample s = { 0.04f, 0.0f, 0.0f, 0.0f, 20.0f };

	weather_field_roughen(&s, 1234.0f, 5678.0f, 9u);
	assert(s.cloud < 0.1f);
}

static void test_cloud_colours_are_valid_and_clear_is_see_through(void) {
	weather_sample clear = { 0.0f, 0.0f, 0.0f, 0.0f, 20.0f };
	weather_sample storm = { 1.0f, 0.9f, 1.0f, 0.0f, 20.0f };
	float c[4];

	weather_field_color(WEATHER_LAYER_CLOUDS, &clear, c);
	assert(c[3] < 0.02f);

	weather_field_color(WEATHER_LAYER_CLOUDS, &storm, c);
	assert(c[3] > 0.6f);
	for(int i = 0; i < 4; i++)
		assert(c[i] >= 0.0f && c[i] <= 1.0f);
}

static void test_rain_is_bluer_than_plain_cloud_and_snow_is_whiter(void) {
	weather_sample cloud = { 0.9f, 0.0f, 0.0f, 0.0f, 10.0f };
	weather_sample rain = { 0.9f, 0.8f, 0.0f, 0.0f, 10.0f };
	weather_sample snow = { 0.9f, 0.0f, 0.0f, 0.9f, -5.0f };
	float a[4];
	float b[4];
	float c[4];

	weather_field_color(WEATHER_LAYER_CLOUDS, &cloud, a);
	weather_field_color(WEATHER_LAYER_CLOUDS, &rain, b);
	weather_field_color(WEATHER_LAYER_CLOUDS, &snow, c);

	assert(b[2] - b[0] > a[2] - a[0]);
	assert(c[0] + c[1] + c[2] > a[0] + a[1] + a[2]);
}

static void test_temperature_runs_cold_to_warm(void) {
	weather_sample cold = { 0.0f, 0.0f, 0.0f, 0.0f, -20.0f };
	weather_sample warm = { 0.0f, 0.0f, 0.0f, 0.0f, 35.0f };
	weather_sample mid = { 0.0f, 0.0f, 0.0f, 0.0f, 10.0f };
	float a[4];
	float b[4];
	float m[4];

	weather_field_color(WEATHER_LAYER_TEMPERATURE, &cold, a);
	weather_field_color(WEATHER_LAYER_TEMPERATURE, &warm, b);
	weather_field_color(WEATHER_LAYER_TEMPERATURE, &mid, m);

	// Cold is bluer, warm is redder, and the layer paints everywhere
	assert(a[2] > a[0] && b[0] > b[2]);
	assert(a[3] > 0.3f && b[3] > 0.3f && m[3] > 0.3f);

	// Beyond the ends is held at the ends
	cold.temp_c = -90.0f;
	weather_field_color(WEATHER_LAYER_TEMPERATURE, &cold, m);
	assert(near(m[0], a[0]) && near(m[2], a[2]));
}

int main(void) {
	test_a_capital_has_its_own_regions_weather();
	test_each_condition_means_what_it_says();
	test_between_two_capitals_it_is_a_mix();
	test_the_blend_is_smooth_not_blocky();
	test_no_regions_is_calm_not_a_crash();
	test_noise_is_in_range_and_repeatable();
	test_noise_varies_and_follows_the_seed();
	test_noise_has_no_jumps();
	test_roughening_keeps_values_in_range();
	test_a_clear_sky_stays_clear_after_roughening();
	test_cloud_colours_are_valid_and_clear_is_see_through();
	test_rain_is_bluer_than_plain_cloud_and_snow_is_whiter();
	test_temperature_runs_cold_to_warm();
	printf("weather_field_test: all passed\n");
	return 0;
}
