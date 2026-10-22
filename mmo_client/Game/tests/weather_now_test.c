// Check the weather of this moment: its targets, its easing, its lightning,
// and what it does to the sky and the light.
#include "world/weather_now.h"
#include "world/weather_state.h"
#include "world/world_lighting.h"
#include "world/sky.h"
#include "protocol.h"
#include "world_regions.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NEAR(a, b, eps) (fabsf((a) - (b)) < (eps))

// Every second of an hour, sampled
#define SAMPLES 3600

// Ennara's middle: where the city is, and where a front reaches first for that region
#define CENTRE_X ((float)WORLD_CITIES[0].tile_x * WORLD_TILE_PX)
#define CENTRE_Y ((float)WORLD_CITIES[0].tile_y * WORLD_TILE_PX)

static WeatherDayData day_of(int condition, int intensity, int wind) {
	WeatherDayData d;

	memset(&d, 0, sizeof(d));
	d.condition = (uint8_t)condition;
	d.intensity = (uint8_t)intensity;
	d.wind = (uint8_t)wind;

	return d;
}

// Force a day of weather and take the weather at many places, one after another
static float mean_over_places(const WeatherDayData* day, int field) {
	float sum = 0.0f;

	weather_state_reset();
	weather_now_reset();
	weather_now_force_day(day, 0);
	for(int i = 0; i < 400; i++) {
		const weather_now* w;

		weather_state_tick(50000.0 + i * 37.0);
		weather_now_update(0.0f, 0, -9000.0f + (float)i * 45.0f, 1200.0f - (float)i * 13.0f);
		w = weather_now_get();
		sum += field == 0 ? w->cloud : field == 1 ? w->rain : field == 2 ? w->snow : w->storm;
	}
	weather_now_force(-1);

	return sum / 400.0f;
}

static void test_each_condition_is_what_it_says(void) {
	WeatherDayData clear = day_of(WEATHER_CLEAR, 0, 0);
	WeatherDayData cloudy = day_of(WEATHER_CLOUDY, 0, 1);
	WeatherDayData rain = day_of(WEATHER_RAIN, 2, 1);
	WeatherDayData storm = day_of(WEATHER_THUNDER, 3, 3);
	WeatherDayData snow = day_of(WEATHER_SNOW, 2, 1);

	assert(mean_over_places(&clear, 0) < 0.2f);
	assert(mean_over_places(&cloudy, 0) > 0.5f);
	assert(mean_over_places(&clear, 1) == 0.0f && mean_over_places(&cloudy, 1) == 0.0f);
	assert(mean_over_places(&rain, 1) > 0.15f);
	assert(mean_over_places(&rain, 2) == 0.0f && mean_over_places(&storm, 2) == 0.0f);
	assert(mean_over_places(&snow, 2) > 0.15f && mean_over_places(&snow, 1) == 0.0f);
	assert(mean_over_places(&storm, 1) > mean_over_places(&rain, 1));
	assert(mean_over_places(&storm, 3) > 0.2f);
	assert(mean_over_places(&rain, 3) == 0.0f);
}

// The lowest rain or snow found at any of many places, over a forced day
static float least_over_places(const WeatherDayData* day, int field) {
	float least = 1.0f;

	weather_state_reset();
	weather_now_reset();
	weather_now_force_day(day, 0);
	for(int i = 0; i < 400; i++) {
		const weather_now* w;
		float value;

		weather_state_tick(50000.0 + i * 37.0);
		weather_now_update(0.0f, 0, -9000.0f + (float)i * 45.0f, 1200.0f - (float)i * 13.0f);
		w = weather_now_get();
		value = field == 1 ? w->rain : w->snow;
		if(value < least)
			least = value;
	}
	weather_now_force(-1);

	return least;
}

static void test_a_forced_fall_is_everywhere(void) {
	WeatherDayData light_rain = day_of(WEATHER_RAIN, 1, 1);
	WeatherDayData snow = day_of(WEATHER_SNOW, 2, 1);

	// Forced to be seen, it falls wherever and whenever you stand, not in patches
	assert(least_over_places(&light_rain, 1) > 0.1f);
	assert(least_over_places(&snow, 2) > 0.2f);
}

static void test_a_harder_fall_is_harder(void) {
	WeatherDayData light = day_of(WEATHER_RAIN, 1, 1);
	WeatherDayData moderate = day_of(WEATHER_RAIN, 2, 1);
	WeatherDayData heavy = day_of(WEATHER_RAIN, 3, 1);
	WeatherDayData light_snow = day_of(WEATHER_SNOW, 1, 1);
	WeatherDayData heavy_snow = day_of(WEATHER_SNOW, 3, 1);

	assert(mean_over_places(&light, 1) < mean_over_places(&moderate, 1));
	assert(mean_over_places(&moderate, 1) < mean_over_places(&heavy, 1));
	assert(mean_over_places(&light_snow, 2) < mean_over_places(&heavy_snow, 2));
}

static void test_the_wind_follows_its_level(void) {
	float speeds[4];

	for(int level = 0; level < 4; level++) {
		WeatherDayData day = day_of(WEATHER_CLOUDY, 0, level);
		float sum = 0.0f;

		weather_state_reset();
		weather_now_reset();
		weather_now_force_day(&day, 0);
		for(int i = 0; i < 300; i++) {
			const weather_now* w;

			weather_state_tick(80000.0 + i * 11.0);
			weather_now_update(1.0f, 0, 0.0f, 0.0f);
			w = weather_now_get();
			sum += sqrtf(w->wind[0] * w->wind[0] + w->wind[1] * w->wind[1]);
			assert(NEAR(w->wind_level, (float)level, 0.01f));
		}
		speeds[level] = sum / 300.0f;
		weather_now_force(-1);
	}
	assert(speeds[0] < speeds[1] && speeds[1] < speeds[2] && speeds[2] < speeds[3]);
	assert(speeds[0] < 15.0f && speeds[3] > 100.0f);
}

static void test_the_wind_blows_the_way_the_weather_travels(void) {
	WeatherDayData day = day_of(WEATHER_RAIN, 2, 3);
	float dir_x;
	float dir_y;
	float along = 0.0f;
	float across = 0.0f;

	weather_cells_drift(&dir_x, &dir_y, NULL);
	weather_state_reset();
	weather_now_reset();
	weather_now_force_day(&day, 0);
	for(int i = 0; i < 300; i++) {
		const weather_now* w;

		weather_state_tick(90000.0 + i * 13.0);
		weather_now_update(1.0f, 0, 0.0f, 0.0f);
		w = weather_now_get();
		along += w->wind[0] * dir_x + w->wind[1] * dir_y;
		across += fabsf(w->wind[0] * dir_y - w->wind[1] * dir_x);
	}
	weather_now_force(-1);
	// It swings a little, but it is a wind from behind the weather, not across it
	assert(along > 0.0f && along > across * 2.0f);
}

static void test_lightning_only_where_a_storm_is_in_sight(void) {
	weather_now w;
	int flashes = 0;
	int bolts = 0;

	for(int i = 0; i < 20000; i++) {
		weather_now_lightning(900.0 + i * 0.05, 0.2f, 0.0f, &w);
		assert(w.flash == 0.0f && w.bolt == 0.0f);
	}
	for(int i = 0; i < 20000; i++) {
		weather_now_lightning(900.0 + i * 0.05, 1.0f, 1.0f, &w);
		assert(w.flash >= 0.0f && w.flash <= 1.0f);
		if(w.flash > 0.5f)
			flashes++;
		if(w.bolt > 0.0f) {
			bolts++;
			assert(w.flash > 0.0f);
			assert(w.bolt_angle >= 0.0f && w.bolt_angle <= 6.29f);
		}
	}
	// 1000 seconds of storm: a good many strikes, not one a frame
	assert(flashes > 20 && flashes < 4000);
	// A bolt shows for a third of a second, so it is up a small part of the time
	assert(bolts > 100 && bolts < 20000 / 5);
}

static void test_a_far_storm_lights_the_sky_less_than_one_overhead(void) {
	weather_now over;
	weather_now away;
	float over_peak = 0.0f;
	float away_peak = 0.0f;

	for(int i = 0; i < 40000; i++) {
		weather_now_lightning(900.0 + i * 0.025, 1.0f, 1.0f, &over);
		weather_now_lightning(900.0 + i * 0.025, 1.0f, 0.0f, &away);
		// The same strikes, brighter overhead
		assert((over.bolt > 0.0f) == (away.bolt > 0.0f));
		if(over.flash > over_peak)
			over_peak = over.flash;
		if(away.flash > away_peak)
			away_peak = away.flash;
	}
	assert(over_peak > 0.9f);
	assert(away_peak > 0.05f && away_peak < 0.3f);
}

static void test_lightning_is_the_same_for_everyone(void) {
	weather_now a;
	weather_now b;

	for(int i = 0; i < 2000; i++) {
		weather_now_lightning(4000.0 + i * 0.1, 0.9f, 0.9f, &a);
		weather_now_lightning(4000.0 + i * 0.1, 0.9f, 0.9f, &b);
		assert(a.flash == b.flash && a.bolt == b.bolt && a.bolt_angle == b.bolt_angle);
	}
}

static WeatherForecast forecast_of(int condition, int intensity, int wind) {
	WeatherForecast f;

	memset(&f, 0, sizeof(f));
	f.day_index = 3400;
	f.day_length_ms = 7200000u;
	f.ms_to_next_day = 7200000u;
	f.region_count = 1;
	for(int d = 0; d < WEATHER_FORECAST_DAYS; d++)
		f.days[0][d] = day_of(condition, intensity, wind);

	return f;
}

// Run the weather for seconds, one second at a time, under a forecast, with the
// player walking nowhere. Returns the clock after.
static double run(int condition, double from_s, int seconds) {
	WeatherForecast f = forecast_of(condition, condition == WEATHER_RAIN || condition == WEATHER_SNOW ? 3 : 0, 1);
	double t = from_s;

	weather_state_apply(&f, t);
	for(int i = 0; i < seconds; i++) {
		t += 1.0;
		weather_state_tick(t);
		weather_now_update(1.0f, 0, 0.0f, 0.0f);
	}

	return t;
}

static void test_the_ground_wets_dries_and_whitens(void) {
	const weather_now* w;
	double t;

	weather_state_reset();
	weather_now_reset();
	weather_now_force(-1);
	t = run(WEATHER_CLEAR, 0.0, 5);
	w = weather_now_get();
	assert(w->wet == 0.0f && w->snow_cover == 0.0f && w->rain == 0.0f);

	// A heavy rain begins: the ground soaks as it falls
	t = run(WEATHER_RAIN, t, 900);
	assert(w->wet > 0.5f);
	assert(w->cloud > 0.6f);

	// It stops: the rain ends soon, the ground takes much longer to dry
	t = run(WEATHER_CLEAR, t, 100);
	assert(w->rain < 0.05f);
	assert(w->wet > 0.3f);
	t = run(WEATHER_CLEAR, t, 800);
	assert(w->wet < 0.05f);

	// Snow lies, and rain washes it off
	t = run(WEATHER_SNOW, t, 1500);
	assert(w->snow_cover > 0.5f);
	t = run(WEATHER_RAIN, t, 1500);
	assert(w->snow_cover < 0.3f);
}

static void test_joining_mid_storm_starts_in_it(void) {
	const weather_now* w;
	int in_it = 0;

	// A heavy storm covers most of the ground: nearly everywhere starts wet
	for(int i = 0; i < 20; i++) {
		WeatherForecast f = forecast_of(WEATHER_THUNDER, 3, 2);

		weather_state_reset();
		weather_now_reset();
		weather_now_force(-1);
		weather_state_apply(&f, 0.0);
		weather_state_tick(1.0 + i * 3.0);
		weather_now_update(0.016f, 0, (float)i * 400.0f, 0.0f);
		w = weather_now_get();
		if(w->rain > 0.1f && w->wet > 0.5f && w->cloud > 0.7f)
			in_it++;
	}
	assert(in_it > 10);
}

// Join a clear day that began into_s seconds ago, after a day of `yesterday` that
// this client saw end (a new forecast over an old one), and take the ground
static const weather_now* join_after(const WeatherForecast* yesterday, double into_s, float spot) {
	WeatherForecast today = forecast_of(WEATHER_CLEAR, 0, 0);

	weather_state_reset();
	weather_now_reset();
	weather_now_force(-1);
	weather_state_apply(yesterday, 0.0);
	today.day_index = yesterday->day_index + 1;
	today.ms_to_next_day = 7200000u - (uint32_t)(into_s * 1000.0);
	weather_state_apply(&today, 10.0);
	weather_state_tick(10.0);
	weather_now_update(0.016f, 0, CENTRE_X + spot * 700.0f, CENTRE_Y);

	return weather_now_get();
}

// The mean ground over twelve different days to join on (the weather's patches
// are not the same on any two), so no one dry spell decides it
static float mean_ground_after(const WeatherForecast* yesterday, double into_s, int snow) {
	float sum = 0.0f;

	for(int i = 0; i < 12; i++) {
		WeatherForecast day = *yesterday;
		const weather_now* w;

		day.day_index += (uint32_t)i * 13u;
		w = join_after(&day, into_s, 0.0f);

		sum += snow ? w->snow_cover : w->wet;
	}

	return sum / 12.0f;
}

static void test_joining_after_rain_finds_the_ground_still_wet(void) {
	WeatherForecast rain = forecast_of(WEATHER_RAIN, 3, 1);
	WeatherForecast snow = forecast_of(WEATHER_SNOW, 3, 1);
	WeatherForecast clear = forecast_of(WEATHER_CLEAR, 0, 0);

	// The rain stopped a few minutes ago: dry now, but the ground has not dried
	assert(mean_ground_after(&rain, 300.0, 0) > 0.3f);
	assert(mean_ground_after(&rain, 300.0, 0) < 0.95f);
	// the same for snow, which melts slowly
	assert(mean_ground_after(&snow, 300.0, 1) > 0.5f);
	// long enough after, there is nothing left of it
	assert(mean_ground_after(&rain, 3000.0, 0) == 0.0f);
	assert(mean_ground_after(&snow, 3000.0, 1) == 0.0f);
	// and a clear day before a clear day leaves a dry ground
	assert(mean_ground_after(&clear, 300.0, 0) == 0.0f);
	assert(mean_ground_after(&clear, 300.0, 1) == 0.0f);
}

static void test_joining_in_a_long_rain_finds_the_ground_soaked(void) {
	WeatherForecast rain = forecast_of(WEATHER_RAIN, 3, 1);
	WeatherForecast snow = forecast_of(WEATHER_SNOW, 3, 1);
	const weather_now* w;

	// An hour into a rainy day the ground is soaked wherever you step in
	for(int i = 0; i < 12; i++) {
		weather_state_reset();
		weather_now_reset();
		weather_now_force(-1);
		rain.ms_to_next_day = 3600000u;
		weather_state_apply(&rain, 0.0);
		weather_state_tick(1.0);
		weather_now_update(0.016f, 0, CENTRE_X + (float)i * 700.0f, CENTRE_Y);
		w = weather_now_get();
		assert(w->wet > 0.6f);
	}
	snow.ms_to_next_day = 3600000u;
	weather_state_reset();
	weather_now_reset();
	weather_now_force(-1);
	weather_state_apply(&snow, 0.0);
	weather_state_tick(1.0);
	weather_now_update(0.016f, 0, CENTRE_X, CENTRE_Y);
	assert(weather_now_get()->snow_cover > 0.6f);
}

static void test_joining_with_no_yesterday_assumes_today(void) {
	WeatherForecast clear = forecast_of(WEATHER_CLEAR, 0, 0);
	const weather_now* w;

	// No yesterday seen, a clear day: the ground is dry
	clear.ms_to_next_day = 7200000u - 300000u;
	weather_state_reset();
	weather_now_reset();
	weather_now_force(-1);
	weather_state_apply(&clear, 0.0);
	weather_state_tick(1.0);
	weather_now_update(0.016f, 0, CENTRE_X, CENTRE_Y);
	w = weather_now_get();
	assert(w->wet == 0.0f && w->snow_cover == 0.0f);
}

static void test_force_overrides_the_forecast(void) {
	const weather_now* w;
	WeatherForecast f = forecast_of(WEATHER_CLEAR, 0, 0);

	weather_state_reset();
	weather_now_reset();
	weather_state_apply(&f, 0.0);
	weather_state_tick(1.0);
	assert(weather_now_forced() == -1);
	weather_now_force(WEATHER_SNOW);
	assert(weather_now_forced() == WEATHER_SNOW);
	weather_now_update(0.016f, 0, 0.0f, 0.0f);
	w = weather_now_get();
	assert(w->cloud > 0.5f);
	weather_now_force(99);
	assert(weather_now_forced() == -1);
	weather_now_force(-1);
}

static void test_no_forecast_is_a_clear_day(void) {
	const weather_now* w;

	weather_state_reset();
	weather_now_reset();
	weather_now_force(-1);
	weather_state_tick(50.0);
	weather_now_update(1.0f, 0, 0.0f, 0.0f);
	w = weather_now_get();
	assert(w->cloud < 0.2f && w->rain == 0.0f && w->snow == 0.0f && w->fog < 0.00001f);
}

static void test_clear_and_partly_have_no_fog(void) {
	assert(weather_fog_density(0.04f, 0.0f, 0.0f, 0.0f) < 0.00001f);
	assert(weather_fog_density(0.38f, 0.0f, 0.0f, 0.0f) < 0.00001f);
	assert(weather_fog_density(1.0f, 0.5f, 0.0f, 0.0f) > weather_fog_density(1.0f, 0.2f, 0.0f, 0.0f));
	assert(weather_fog_density(1.0f, 0.0f, 0.5f, 0.0f) > weather_fog_density(1.0f, 0.5f, 0.0f, 0.0f));
}

static void test_a_new_day_rolls_in_over_the_old_one(void) {
	// Yesterday was clear, today is heavy rain: at the start of the day the
	// ground is dry, and a few minutes later it is raining
	const weather_now* w;
	WeatherForecast clear = forecast_of(WEATHER_CLEAR, 0, 0);
	WeatherForecast rain = forecast_of(WEATHER_RAIN, 3, 2);
	double t = 0.0;
	float rain_at_start;
	float rain_later = 0.0f;

	weather_state_reset();
	weather_now_reset();
	weather_now_force(-1);
	weather_state_apply(&clear, t);
	// The new day: the forecast moves on by one, and remembers the day before
	rain.day_index = clear.day_index + 1;
	weather_state_apply(&rain, t);
	for(int i = 0; i < 5; i++) {
		t += 1.0;
		weather_state_tick(t);
		weather_now_update(1.0f, 0, CENTRE_X, CENTRE_Y);
	}
	w = weather_now_get();
	rain_at_start = w->rain;
	// The rain comes in cells that travel in: a new day does not start in full rain
	for(int i = 0; i < 1500; i++) {
		t += 1.0;
		weather_state_tick(t);
		weather_now_update(1.0f, 0, CENTRE_X, CENTRE_Y);
		if(w->rain > rain_later)
			rain_later = w->rain;
	}
	assert(rain_at_start < 0.3f);
	assert(rain_later > 0.3f);
}

static void test_a_rolling_force_arrives_as_a_front(void) {
	const weather_now* w;
	WeatherDayData storm = day_of(WEATHER_RAIN, 3, 2);
	double t = 100.0;
	float first = -1.0f;

	weather_state_reset();
	weather_now_reset();
	weather_now_force(-1);
	weather_state_tick(t);
	weather_now_update(1.0f, 0, CENTRE_X, CENTRE_Y);
	weather_now_force_day(&storm, 1);
	for(int i = 0; i < 600; i++) {
		t += 1.0;
		weather_state_tick(t);
		weather_now_update(1.0f, 0, CENTRE_X, CENTRE_Y);
		w = weather_now_get();
		if(first < 0.0f && w->rain > 0.1f)
			first = (float)i;
		if(i == 0)
			assert(w->rain == 0.0f);
	}
	// It takes a couple of minutes to reach the player, and then it is there
	assert(first > 60.0f && first < 400.0f);
	weather_now_force(-1);
}

// A scripted storm seen from where the player stands, a second at a time
static void test_a_scripted_storm_rolls_through_and_ends_clear(void) {
	const weather_now* w;
	double t = 100000.0;
	float last = 0.0f;
	float biggest = 0.0f;
	float peak = 0.0f;
	float wettest = 0.0f;
	double elapsed;
	double length;
	int thunder_seen = 0;

	for(int thunder = 0; thunder < 2; thunder++) {
		weather_state_reset();
		weather_now_reset();
		weather_now_force(-1);
		weather_state_tick(t);
		weather_now_update(1.0f, 0, CENTRE_X, CENTRE_Y);
		weather_now_start_storm(480.0, thunder);
		last = 0.0f;
		biggest = 0.0f;
		peak = 0.0f;
		for(int i = 0; i < 520; i++) {
			t += 1.0;
			weather_state_tick(t);
			weather_now_update(1.0f, 0, CENTRE_X, CENTRE_Y);
			w = weather_now_get();
			if(i == 5) {
				// Clear to begin with
				assert(w->rain < 0.02f);
				assert(weather_now_storm_progress(&elapsed, &length) && length == 480.0);
			}
			if(i > 20 && fabsf(w->rain - last) > biggest)
				biggest = fabsf(w->rain - last);
			if(w->rain > peak)
				peak = w->rain;
			if(w->wet > wettest)
				wettest = w->wet;
			if(w->storm_near > 0.3f)
				thunder_seen++;
			last = w->rain;
		}
		// Heavy at its height, no jump on the way, and clear at the end
		assert(peak > 0.7f);
		assert(biggest < 0.06f);
		assert(wettest > 0.3f);
		assert(w->rain < 0.01f && w->cloud < 0.2f);
		assert(!weather_now_storm_progress(&elapsed, &length));
		assert(weather_now_forced() == WEATHER_CLEAR);
		if(!thunder)
			assert(thunder_seen == 0);
	}
	assert(thunder_seen > 0);
	weather_now_force(-1);
}

// A storm called up, seen from where the player stands, a second at a time
static void test_a_called_storm_comes_to_the_player_and_passes(void) {
	const weather_now* w;
	double t = 200000.0;
	float broad_wind = 0.0f;

	for(int sharp = 0; sharp < 2; sharp++) {
		float peak = 0.0f;
		float wind = 0.0f;
		float last = 0.0f;
		float biggest = 0.0f;
		int cloud_at = -1;
		int rain_at = -1;
		int peak_at = 0;

		weather_state_reset();
		weather_now_reset();
		weather_now_force(-1);
		weather_state_tick(t);
		weather_now_update(1.0f, 0, CENTRE_X, CENTRE_Y);
		weather_now_start_cell(sharp, 1);
		assert(weather_now_cell_running());
		for(int i = 0; i < 1000; i++) {
			t += 1.0;
			weather_state_tick(t);
			weather_now_update(1.0f, 0, CENTRE_X, CENTRE_Y);
			w = weather_now_get();
			if(i < 5)
				assert(w->rain < 0.02f);
			if(cloud_at < 0 && w->cloud > 0.6f)
				cloud_at = i;
			if(rain_at < 0 && w->rain > 0.1f)
				rain_at = i;
			if(w->rain > peak) {
				peak = w->rain;
				peak_at = i;
			}
			if(w->wind_level > wind)
				wind = w->wind_level;
			if(i > 10 && fabsf(w->rain - last) > biggest)
				biggest = fabsf(w->rain - last);
			last = w->rain;
		}
		// It is seen coming, its middle is over the player 2 to 5 minutes after it is called, and it passes
		assert(cloud_at >= 0 && rain_at > cloud_at);
		assert(peak > 0.8f && abs(peak_at - (sharp ? 150 : 300)) < 25);
		assert(w->rain < 0.01f && !weather_now_cell_running());
		assert(weather_now_forced() == WEATHER_CLEAR);
		// The wind comes up with it, harder for the sharp one
		assert(wind > (sharp ? 2.2f : 1.6f));
		if(sharp)
			assert(wind > broad_wind + 0.4f && biggest > 0.02f);
		else {
			assert(biggest < 0.05f);
			broad_wind = wind;
		}
	}
	weather_now_force(-1);
}

static float luminance(const float* c) {
	return 0.30f * c[0] + 0.59f * c[1] + 0.11f * c[2];
}

static void test_clear_weather_leaves_sky_and_light_alone(void) {
	weather_now clear;
	sky_state a;
	sky_state b;
	world_lighting la;
	world_lighting lb;

	memset(&clear, 0, sizeof(clear));
	for(float hour = 0.0f; hour < 24.0f; hour += 1.5f) {
		sky_at(hour, &a);
		b = a;
		sky_apply_weather(&b, &clear);
		assert(memcmp(&a, &b, sizeof(a)) == 0);
		world_lighting_from_sky(&a, &la);
		lb = la;
		world_lighting_apply_weather(&lb, &clear);
		assert(memcmp(&la, &lb, sizeof(la)) == 0);
	}
}

static void test_overcast_greys_and_darkens_the_sky(void) {
	weather_now storm;
	sky_state a;
	sky_state b;

	memset(&storm, 0, sizeof(storm));
	storm.cloud = 1.0f;
	storm.storm = 1.0f;
	sky_at(12.0f, &a);
	b = a;
	sky_apply_weather(&b, &storm);
	assert(luminance(b.zenith) < luminance(a.zenith) * 0.8f);
	assert(luminance(b.horizon) < luminance(a.horizon) * 0.8f);
	// Less blue than before: nearer grey
	assert(b.zenith[2] - b.zenith[0] < (a.zenith[2] - a.zenith[0]) * 0.5f);
	// The haze below the horizon follows the horizon
	assert(NEAR(b.haze[1], b.horizon[1] * 0.85f, 0.001f));
	// Stars are hidden behind cloud
	sky_at(0.0f, &a);
	b = a;
	sky_apply_weather(&b, &storm);
	assert(a.star_amount > 0.9f && b.star_amount < 0.1f);
}

static void test_lightning_lights_the_sky(void) {
	weather_now flash;
	sky_state a;
	sky_state b;

	memset(&flash, 0, sizeof(flash));
	flash.cloud = 1.0f;
	sky_at(0.0f, &a);
	b = a;
	sky_apply_weather(&b, &flash);
	a = b;
	flash.flash = 1.0f;
	sky_apply_weather(&b, &flash);
	assert(luminance(b.zenith) > luminance(a.zenith) + 0.3f);
}

static void test_overcast_dims_the_sun_and_drops_its_shadows(void) {
	weather_now over;
	world_lighting a;
	world_lighting b;
	sky_state sky;

	memset(&over, 0, sizeof(over));
	over.cloud = 1.0f;
	sky_at(12.0f, &sky);
	world_lighting_from_sky(&sky, &a);
	b = a;
	world_lighting_apply_weather(&b, &over);
	assert(b.color[1] < a.color[1] * 0.5f);
	assert(b.shadow_strength < 0.05f);
	assert(a.shadow_strength > 0.9f);
	// Darker overall, but nowhere near night
	assert(luminance(b.ambient_up) < luminance(a.ambient_up));
	assert(luminance(b.ambient_up) > luminance(a.ambient_up) * 0.6f);
}

static void test_partly_cloudy_keeps_most_of_the_sun(void) {
	weather_now partly;
	world_lighting a;
	world_lighting b;
	sky_state sky;

	memset(&partly, 0, sizeof(partly));
	partly.cloud = 0.38f;
	sky_at(12.0f, &sky);
	world_lighting_from_sky(&sky, &a);
	b = a;
	world_lighting_apply_weather(&b, &partly);
	assert(b.color[1] > a.color[1] * 0.85f);
	assert(b.shadow_strength > 0.8f);
}

static void test_flash_and_snow_brighten_the_ambient(void) {
	weather_now w;
	world_lighting a;
	world_lighting b;
	sky_state sky;

	memset(&w, 0, sizeof(w));
	w.cloud = 1.0f;
	sky_at(0.0f, &sky);
	world_lighting_from_sky(&sky, &a);
	world_lighting_apply_weather(&a, &w);
	b = a;
	w.flash = 1.0f;
	b = a;
	world_lighting_from_sky(&sky, &b);
	world_lighting_apply_weather(&b, &w);
	assert(luminance(b.ambient_up) > luminance(a.ambient_up) + 0.3f);

	w.flash = 0.0f;
	world_lighting_from_sky(&sky, &a);
	world_lighting_apply_weather(&a, &w);
	w.snow_cover = 1.0f;
	world_lighting_from_sky(&sky, &b);
	world_lighting_apply_weather(&b, &w);
	assert(luminance(b.ambient_up) > luminance(a.ambient_up) * 1.2f);
}

static void test_the_cloud_clock_ticks_steadily_whatever_the_wind_does(void) {
	// The clouds' drift is their clock times a fixed speed, so a gust cannot
	// make them lurch: the clock moves one second a second
	for(int level = 0; level < 4; level++) {
		WeatherDayData day = day_of(WEATHER_CLOUDY, 0, level);

		weather_state_reset();
		weather_now_reset();
		weather_now_force_day(&day, 0);
		for(int i = 0; i < 200; i++) {
			float before;

			weather_state_tick(70000.0 + i * 13.0);
			weather_now_update(0.1f, 0, 0.0f, 0.0f);
			before = weather_now_get()->cloud_time_s;
			weather_state_tick(70000.0 + i * 13.0 + 1.0);
			weather_now_update(0.1f, 0, 0.0f, 0.0f);
			assert(NEAR(weather_now_get()->cloud_time_s - before, 1.0f, 0.05f));
		}
		weather_now_force(-1);
	}
}

static void test_a_stormy_sky_turns_the_lamps_on_by_day(void) {
	weather_now w;
	world_lighting clear;
	world_lighting rain;
	world_lighting storm;
	world_lighting snow;
	world_lighting night;
	sky_state sky;

	memset(&w, 0, sizeof(w));
	sky_at(12.0f, &sky);
	world_lighting_from_sky(&sky, &clear);
	rain = clear;
	storm = clear;
	snow = clear;
	world_lighting_apply_weather(&clear, &w);
	assert(clear.lamp_amount == 0.0f);
	w.cloud = 0.92f;
	w.rain = 0.55f;
	world_lighting_apply_weather(&rain, &w);
	w.cloud = 1.0f;
	w.rain = 0.85f;
	w.storm = 1.0f;
	world_lighting_apply_weather(&storm, &w);
	memset(&w, 0, sizeof(w));
	w.cloud = 0.88f;
	w.snow = 0.6f;
	world_lighting_apply_weather(&snow, &w);
	assert(rain.lamp_amount > 0.1f && rain.lamp_amount < storm.lamp_amount);
	assert(storm.lamp_amount > 0.7f && storm.lamp_amount <= 1.0f);
	assert(snow.lamp_amount < 0.3f);
	// At night they were already on, and the weather does not turn them down
	sky_at(0.0f, &sky);
	world_lighting_from_sky(&sky, &night);
	memset(&w, 0, sizeof(w));
	w.cloud = 1.0f;
	world_lighting_apply_weather(&night, &w);
	assert(night.lamp_amount == 1.0f);
}

int main(void) {
	test_each_condition_is_what_it_says();
	test_a_forced_fall_is_everywhere();
	test_a_harder_fall_is_harder();
	test_the_wind_follows_its_level();
	test_the_wind_blows_the_way_the_weather_travels();
	test_clear_and_partly_have_no_fog();
	test_lightning_only_where_a_storm_is_in_sight();
	test_a_far_storm_lights_the_sky_less_than_one_overhead();
	test_lightning_is_the_same_for_everyone();
	test_the_ground_wets_dries_and_whitens();
	test_a_new_day_rolls_in_over_the_old_one();
	test_a_rolling_force_arrives_as_a_front();
	test_a_scripted_storm_rolls_through_and_ends_clear();
	test_a_called_storm_comes_to_the_player_and_passes();
	test_joining_mid_storm_starts_in_it();
	test_joining_after_rain_finds_the_ground_still_wet();
	test_joining_in_a_long_rain_finds_the_ground_soaked();
	test_joining_with_no_yesterday_assumes_today();
	test_force_overrides_the_forecast();
	test_no_forecast_is_a_clear_day();
	test_clear_weather_leaves_sky_and_light_alone();
	test_overcast_greys_and_darkens_the_sky();
	test_lightning_lights_the_sky();
	test_overcast_dims_the_sun_and_drops_its_shadows();
	test_partly_cloudy_keeps_most_of_the_sun();
	test_flash_and_snow_brighten_the_ambient();
	test_the_cloud_clock_ticks_steadily_whatever_the_wind_does();
	test_a_stormy_sky_turns_the_lamps_on_by_day();
	printf("weather_now_test: all passed\n");

	return 0;
}
