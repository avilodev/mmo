// Check the patches of weather: how much ground each kind of day covers, that
// they drift and come and go, and that a new day arrives as a front.
#include "world/weather_cells.h"
#include "protocol.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define NEAR(a, b, eps) (fabsf((a) - (b)) < (eps))

static WeatherDayData day_of(int condition, int intensity, int wind) {
	WeatherDayData d;

	memset(&d, 0, sizeof(d));
	d.condition = (uint8_t)condition;
	d.intensity = (uint8_t)intensity;
	d.wind = (uint8_t)wind;

	return d;
}

static weather_cells_input input_for(const WeatherDayData* today) {
	weather_cells_input in;

	memset(&in, 0, sizeof(in));
	weather_envelope_of(today, &in.today);
	in.shared_s = 24000000.0;
	in.into_day_s = 3600.0;

	return in;
}

// The share of a wide stretch of ground, over several minutes, that is wet
static float wet_share(const WeatherDayData* day, float threshold) {
	weather_cells_input in = input_for(day);
	int wet = 0;
	int total = 0;

	for(int t = 0; t < 12; t++) {
		in.shared_s = 24000000.0 + t * 97.0;
		for(int j = 0; j < 40; j++)
			for(int i = 0; i < 40; i++) {
				weather_cell_sample s;

				weather_cells_sample(&in, -20000.0f + (float)i * 1000.0f,
				                     -20000.0f + (float)j * 1000.0f, &s);
				if(s.rain > threshold || s.snow > threshold)
					wet++;
				total++;
			}
	}

	return (float)wet / (float)total;
}

static void test_samples_repeat_and_stay_in_range(void) {
	WeatherDayData days[3] = { day_of(WEATHER_RAIN, 2, 1), day_of(WEATHER_THUNDER, 3, 3),
	                           day_of(WEATHER_SNOW, 1, 0) };

	for(int d = 0; d < 3; d++) {
		weather_cells_input in = input_for(&days[d]);

		for(int i = 0; i < 400; i++) {
			weather_cell_sample a;
			weather_cell_sample b;
			float x = -9000.0f + (float)i * 47.0f;
			float y = 3000.0f - (float)i * 31.0f;

			weather_cells_sample(&in, x, y, &a);
			weather_cells_sample(&in, x, y, &b);
			assert(memcmp(&a, &b, sizeof(a)) == 0);
			assert(a.cloud >= 0.0f && a.cloud <= 1.0f);
			assert(a.rain >= 0.0f && a.rain <= 1.0f);
			assert(a.snow >= 0.0f && a.snow <= 1.0f);
			assert(a.storm >= 0.0f && a.storm <= 1.0f);
			assert(a.wind >= 0.0f && a.wind <= 3.0f);
		}
	}
}

static void test_a_heavier_day_wets_more_of_the_ground(void) {
	float light = wet_share(&(WeatherDayData){ WEATHER_RAIN, 0, 0, 0, 0, 1, 1, { 0 } }, 0.02f);
	float moderate = wet_share(&(WeatherDayData){ WEATHER_RAIN, 0, 0, 0, 0, 2, 1, { 0 } }, 0.02f);
	float heavy = wet_share(&(WeatherDayData){ WEATHER_RAIN, 0, 0, 0, 0, 3, 1, { 0 } }, 0.02f);

	assert(light > 0.25f && light < 0.60f);
	assert(moderate > 0.50f && moderate < 0.85f);
	assert(heavy > 0.80f);
	assert(light < moderate && moderate < heavy);
}

static void test_a_heavier_day_falls_harder(void) {
	float mean[3] = { 0.0f, 0.0f, 0.0f };

	for(int level = 1; level <= 3; level++) {
		WeatherDayData day = day_of(WEATHER_RAIN, level, 1);
		weather_cells_input in = input_for(&day);
		int count = 0;

		for(int j = 0; j < 40; j++)
			for(int i = 0; i < 40; i++) {
				weather_cell_sample s;

				weather_cells_sample(&in, -20000.0f + (float)i * 1000.0f,
				                     -20000.0f + (float)j * 1000.0f, &s);
				if(s.rain > 0.02f) {
					mean[level - 1] += s.rain;
					count++;
				}
			}
		mean[level - 1] /= (float)count;
	}
	// Where it rains: light is a drizzle, heavy a downpour
	assert(mean[0] < 0.35f);
	assert(mean[1] > 0.35f && mean[1] < 0.70f);
	assert(mean[2] > 0.70f);
}

static void test_a_spot_sees_the_rain_come_and_go(void) {
	WeatherDayData day = day_of(WEATHER_RAIN, 1, 1);
	weather_cells_input in = input_for(&day);
	int dry = 0;
	int wet = 0;

	// A light rain, a point watched for an hour of real time
	for(int t = 0; t < 3600; t += 5) {
		weather_cell_sample s;

		in.shared_s = 24000000.0 + t;
		weather_cells_sample(&in, 1200.0f, -800.0f, &s);
		if(s.rain > 0.02f)
			wet++;
		else
			dry++;
	}
	assert(dry > 720 * 15 / 100 && wet > 720 * 15 / 100);
}

static void test_a_heavy_day_is_wet_nearly_all_the_time(void) {
	WeatherDayData day = day_of(WEATHER_RAIN, 3, 1);
	weather_cells_input in = input_for(&day);
	int wet = 0;

	for(int t = 0; t < 3600; t += 5) {
		weather_cell_sample s;

		in.shared_s = 24000000.0 + t;
		weather_cells_sample(&in, 1200.0f, -800.0f, &s);
		if(s.rain > 0.02f)
			wet++;
	}
	assert(wet > 720 * 80 / 100);
}

static void test_the_patches_drift_along_the_wind(void) {
	WeatherDayData day = day_of(WEATHER_RAIN, 2, 1);
	weather_cells_input in = input_for(&day);
	float dx;
	float dy;
	float speed;

	weather_cells_drift(&dx, &dy, &speed);
	assert(NEAR(sqrtf(dx * dx + dy * dy), 1.0f, 0.001f));
	assert(speed > 10.0f && speed < 200.0f);
	for(int i = 0; i < 200; i++) {
		weather_cell_sample here;
		weather_cell_sample later;
		float x = -6000.0f + (float)i * 53.0f;
		float y = 2000.0f - (float)i * 29.0f;
		double gap = 37.0 + (double)i;

		in.shared_s = 24000000.0;
		weather_cells_sample(&in, x, y, &here);
		in.shared_s = 24000000.0 + gap;
		weather_cells_sample(&in, x + dx * speed * (float)gap, y + dy * speed * (float)gap, &later);
		assert(NEAR(here.rain, later.rain, 0.01f));
		assert(NEAR(here.cloud, later.cloud, 0.01f));
	}
}

static void test_the_pattern_is_not_sensitive_to_how_late_the_clock_is(void) {
	// The clock is huge; positions far out must not lose their detail
	WeatherDayData day = day_of(WEATHER_RAIN, 2, 1);
	weather_cells_input in = input_for(&day);
	int changed = 0;

	for(int i = 0; i < 100; i++) {
		weather_cell_sample a;
		weather_cell_sample b;

		in.shared_s = 24000000.0 + (double)i;
		weather_cells_sample(&in, 500.0f, 500.0f, &a);
		in.shared_s = 24000000.0 + (double)i + 0.25;
		weather_cells_sample(&in, 500.0f, 500.0f, &b);
		if(fabsf(a.rain - b.rain) > 0.0001f || fabsf(a.cloud - b.cloud) > 0.0001f)
			changed++;
		// A quarter of a second moves the weather very little
		assert(fabsf(a.rain - b.rain) < 0.03f);
	}
	assert(changed > 30);
}

static void test_only_the_right_kind_of_day_makes_each_fall(void) {
	WeatherDayData rain = day_of(WEATHER_RAIN, 3, 1);
	WeatherDayData snow = day_of(WEATHER_SNOW, 3, 1);
	WeatherDayData cloudy = day_of(WEATHER_CLOUDY, 0, 1);
	WeatherDayData thunder = day_of(WEATHER_THUNDER, 3, 2);
	weather_cells_input in_rain = input_for(&rain);
	weather_cells_input in_snow = input_for(&snow);
	weather_cells_input in_cloudy = input_for(&cloudy);
	weather_cells_input in_thunder = input_for(&thunder);
	float storm_seen = 0.0f;

	for(int i = 0; i < 600; i++) {
		weather_cell_sample s;
		float x = -15000.0f + (float)i * 53.0f;

		weather_cells_sample(&in_rain, x, 100.0f, &s);
		assert(s.snow == 0.0f && s.storm == 0.0f);
		weather_cells_sample(&in_snow, x, 100.0f, &s);
		assert(s.rain == 0.0f && s.storm == 0.0f);
		weather_cells_sample(&in_cloudy, x, 100.0f, &s);
		assert(s.rain == 0.0f && s.snow == 0.0f && s.storm == 0.0f);
		weather_cells_sample(&in_thunder, x, 100.0f, &s);
		assert(s.snow == 0.0f);
		if(s.storm > storm_seen)
			storm_seen = s.storm;
	}
	assert(storm_seen > 0.8f);
}

static void test_a_dry_cloudy_day_has_thick_and_thin_places(void) {
	WeatherDayData day = day_of(WEATHER_CLOUDY, 0, 1);
	weather_cells_input in = input_for(&day);
	float lo = 1.0f;
	float hi = 0.0f;

	for(int i = 0; i < 500; i++) {
		weather_cell_sample s;

		weather_cells_sample(&in, -15000.0f + (float)i * 61.0f, 700.0f, &s);
		if(s.cloud < lo)
			lo = s.cloud;
		if(s.cloud > hi)
			hi = s.cloud;
	}
	assert(hi - lo > 0.3f);
}

static void test_without_a_known_yesterday_the_new_weather_is_everywhere(void) {
	WeatherDayData day = day_of(WEATHER_RAIN, 3, 2);
	weather_cells_input in = input_for(&day);

	in.into_day_s = 0.0;
	for(int i = 0; i < 100; i++) {
		weather_cell_sample s;

		weather_cells_sample(&in, -5000.0f + (float)i * 100.0f, 0.0f, &s);
		assert(s.fresh == 1.0f);
	}
}

// The first second, after the day began, that the new weather has reached a spot
static double arrival_at(const weather_cells_input* base, float x, float y) {
	weather_cells_input in = *base;

	for(double t = 0.0; t < 900.0; t += 1.0) {
		weather_cell_sample s;

		in.into_day_s = t;
		weather_cells_sample(&in, x, y, &s);
		if(s.fresh > 0.5f)
			return t;
	}

	return 900.0;
}

static void test_a_new_day_rolls_in_as_a_front_along_the_wind(void) {
	WeatherDayData today = day_of(WEATHER_RAIN, 3, 3);
	WeatherDayData before = day_of(WEATHER_CLEAR, 0, 0);
	weather_cells_input in = input_for(&today);
	float dx;
	float dy;
	double upwind;
	double middle;
	double downwind;

	weather_envelope_of(&before, &in.before);
	in.has_before = 1;
	in.centre_x = 100.0f;
	in.centre_y = -50.0f;
	weather_cells_drift(&dx, &dy, NULL);
	// Nothing new has reached the middle of the region when the day starts
	{
		weather_cell_sample s;

		in.into_day_s = 0.0;
		weather_cells_sample(&in, in.centre_x, in.centre_y, &s);
		assert(s.fresh < 0.01f);
	}
	// It reaches the upwind side first, then the middle, then the downwind side
	upwind = arrival_at(&in, in.centre_x - dx * 3000.0f, in.centre_y - dy * 3000.0f);
	middle = arrival_at(&in, in.centre_x, in.centre_y);
	downwind = arrival_at(&in, in.centre_x + dx * 3000.0f, in.centre_y + dy * 3000.0f);
	assert(upwind < middle && middle < downwind);
	// At the drift speed, 3000 units takes about a minute
	assert(middle - upwind > 50.0 && middle - upwind < 75.0);
	// The middle is reached a couple of minutes into the day
	assert(middle > 100.0 && middle < 220.0);
	// And it is a front, not a flip: the change takes some time at one spot
	{
		weather_cells_input t = in;
		double first = -1.0;
		double last = -1.0;

		for(double s = 0.0; s < 600.0; s += 1.0) {
			weather_cell_sample sample;

			t.into_day_s = s;
			weather_cells_sample(&t, in.centre_x, in.centre_y, &sample);
			if(first < 0.0 && sample.fresh > 0.1f)
				first = s;
			if(last < 0.0 && sample.fresh > 0.9f)
				last = s;
		}
		assert(last - first > 15.0);
	}
}

// Seconds into a new day when the cloud, and then the rain, reach a spot
static void front_times(float x, float y, double* cloud_at, double* rain_at) {
	WeatherDayData today = day_of(WEATHER_RAIN, 3, 3);
	WeatherDayData before = day_of(WEATHER_CLEAR, 0, 0);
	weather_cells_input in = input_for(&today);

	weather_envelope_of(&before, &in.before);
	in.has_before = 1;
	in.centre_x = 0.0f;
	in.centre_y = 0.0f;
	*cloud_at = -1.0;
	*rain_at = -1.0;
	for(double s = 0.0; s < 600.0; s += 1.0) {
		weather_cell_sample sample;

		in.into_day_s = s;
		weather_cells_sample(&in, x, y, &sample);
		if(*cloud_at < 0.0 && sample.cloud > 0.6f)
			*cloud_at = s;
		if(*rain_at < 0.0 && sample.rain > 0.2f)
			*rain_at = s;
	}
}

static void test_the_cloud_of_a_front_comes_before_its_rain(void) {
	float dx;
	float dy;
	int counted = 0;
	double lead_sum = 0.0;

	weather_cells_drift(&dx, &dy, NULL);
	for(int k = 0; k < 8; k++) {
		double cloud_at;
		double rain_at;

		front_times(dx * (float)(k * 700 - 2500), dy * (float)(k * 700 - 2500) + (float)(k * 90), &cloud_at,
		            &rain_at);
		if(cloud_at < 0.0 || rain_at < 0.0)
			continue;
		assert(cloud_at < rain_at);
		lead_sum += rain_at - cloud_at;
		counted++;
	}
	// Where the rain does fall, the cloud is there a good minute ahead of it
	assert(counted >= 4);
	assert(lead_sum / (double)counted > 55.0 && lead_sum / (double)counted < 110.0);
}

static void test_cloud_spreads_wider_than_rain(void) {
	WeatherDayData day = day_of(WEATHER_RAIN, 1, 1);
	weather_cells_input in = input_for(&day);
	int rainy = 0;
	int shut = 0;
	int rainy_but_open = 0;

	for(int t = 0; t < 8; t++) {
		in.shared_s = 24000000.0 + t * 211.0;
		for(int j = 0; j < 50; j++)
			for(int i = 0; i < 50; i++) {
				weather_cell_sample s;

				weather_cells_sample(&in, -9000.0f + (float)i * 360.0f, -9000.0f + (float)j * 360.0f, &s);
				rainy += s.rain > 0.1f;
				shut += s.cloud > 0.7f;
				rainy_but_open += s.rain > 0.1f && s.cloud < 0.6f;
			}
	}
	// Rain only falls from thick cloud, and the thick cloud covers a wider area
	assert(rainy > 300);
	assert(rainy_but_open == 0);
	assert(shut > rainy * 13 / 10);
}

static void test_behind_the_front_the_old_weather_is_gone(void) {
	WeatherDayData today = day_of(WEATHER_CLEAR, 0, 0);
	WeatherDayData before = day_of(WEATHER_RAIN, 3, 2);
	weather_cells_input in = input_for(&today);
	float old_wet = 0.0f;
	float new_wet = 0.0f;

	weather_envelope_of(&before, &in.before);
	in.has_before = 1;
	for(int i = 0; i < 100; i++) {
		weather_cell_sample s;

		in.into_day_s = 0.0;
		weather_cells_sample(&in, -3000.0f + (float)i * 60.0f, 0.0f, &s);
		old_wet += s.rain;
		in.into_day_s = 900.0;
		weather_cells_sample(&in, -3000.0f + (float)i * 60.0f, 0.0f, &s);
		new_wet += s.rain;
	}
	assert(old_wet > 20.0f);
	assert(new_wet == 0.0f);
}

static void test_a_storm_can_be_seen_from_where_it_is_not_overhead(void) {
	WeatherDayData day = day_of(WEATHER_THUNDER, 3, 3);
	weather_cells_input in = input_for(&day);
	int found = 0;

	for(int i = 0; i < 300; i++) {
		weather_cell_sample here;
		float x = -9000.0f + (float)i * 61.0f;
		float near_storm;

		weather_cells_sample(&in, x, 0.0f, &here);
		near_storm = weather_cells_storm_near(&in, x, 0.0f, 4000.0f);
		assert(near_storm >= here.storm - 0.0001f);
		if(here.storm < 0.1f && near_storm > 0.5f)
			found++;
	}
	assert(found > 20);
}

static void test_the_grid_is_the_samples(void) {
	WeatherDayData day = day_of(WEATHER_RAIN, 2, 1);
	weather_cells_input in = input_for(&day);
	weather_cell_sample grid[5 * 5];

	weather_cells_grid(&in, -250.0f, 100.0f, 125.0f, 5, grid);
	for(int j = 0; j < 5; j++)
		for(int i = 0; i < 5; i++) {
			weather_cell_sample s;

			weather_cells_sample(&in, -250.0f + (float)i * 125.0f, 100.0f + (float)j * 125.0f, &s);
			assert(memcmp(&s, &grid[j * 5 + i], sizeof(s)) == 0);
		}
}

static void test_winds_get_faster_by_level(void) {
	float last = 0.0f;

	for(float level = 0.0f; level <= 4.0f; level += 0.25f) {
		assert(weather_wind_speed(level) >= last);
		last = weather_wind_speed(level);
	}
	assert(weather_wind_speed(0.0f) < 15.0f);
	assert(weather_wind_speed(3.0f) > 150.0f);
	assert(weather_wind_speed(-4.0f) == weather_wind_speed(0.0f));
	assert(weather_wind_speed(4.0f) > weather_wind_speed(3.0f) + 50.0f);
	assert(weather_wind_speed(9.0f) == weather_wind_speed(4.0f));
}

// A called storm seen from a spot, a second at a time: when the cloud is thick, when
// the rain starts, and how hard it gets, and how fast it climbs
typedef struct {
	double cloud_at; // cloud over 0.6
	double rain_at; // rain over 0.1
	double peak_at;
	float peak;
	float wind_peak;
	double wind_peak_at;
	double rise_s; // from a tenth of its height to nine tenths
	double gone_s; // when the rain is back under 0.05
	float biggest_step; // the most rain changes in a second
} storm_run;

static storm_run run_called_storm(int sharp) {
	WeatherDayData clear = day_of(WEATHER_CLEAR, 0, 1);
	weather_cells_input in = input_for(&clear);
	weather_cell cell;
	storm_run r = { -1.0, -1.0, -1.0, 0.0f, 0.0f, -1.0, 0.0, -1.0, 0.0f };
	double ten = -1.0;
	double ninety = -1.0;
	float last = 0.0f;

	in.centre_x = 0.0f;
	in.centre_y = 0.0f;
	weather_cell_called(&in, sharp, 1, in.shared_s + 600.0, 700.0f, 400.0f, &cell);
	in.extra_on = 1;
	in.extra = cell;
	for(int t = 0; t < 1800; t++) {
		weather_cell_sample s;
		double at = (double)t;

		in.shared_s = 24000000.0 + at;
		weather_cells_sample(&in, 700.0f, 400.0f, &s);
		if(r.cloud_at < 0.0 && s.cloud > 0.6f)
			r.cloud_at = at;
		if(r.rain_at < 0.0 && s.rain > 0.1f)
			r.rain_at = at;
		if(s.rain > r.peak) {
			r.peak = s.rain;
			r.peak_at = at;
		}
		if(s.wind > r.wind_peak) {
			r.wind_peak = s.wind;
			r.wind_peak_at = at;
		}
		if(t > 0 && fabsf(s.rain - last) > r.biggest_step)
			r.biggest_step = fabsf(s.rain - last);
		if(ten < 0.0 && s.rain > 0.1f * 0.9f)
			ten = at;
		if(ninety < 0.0 && s.rain > 0.9f * 0.9f)
			ninety = at;
		if(r.peak_at >= 0.0 && at > r.peak_at && r.gone_s < 0.0 && s.rain < 0.05f)
			r.gone_s = at;
		last = s.rain;
	}
	r.rise_s = ninety - ten;

	return r;
}

static void test_a_called_storm_comes_in_goes_over_and_passes(void) {
	storm_run broad = run_called_storm(0);
	storm_run sharp = run_called_storm(1);

	// Both: cloud first, then rain, hardest where its middle is, then it passes
	assert(broad.cloud_at >= 0.0 && broad.rain_at > broad.cloud_at + 60.0);
	assert(sharp.cloud_at >= 0.0 && sharp.rain_at > sharp.cloud_at + 25.0);
	assert(fabs(broad.peak_at - 600.0) < 5.0 && fabs(sharp.peak_at - 600.0) < 5.0);
	assert(broad.peak > 0.85f && sharp.peak > 0.9f);
	assert(broad.gone_s > broad.peak_at + 60.0 && sharp.gone_s > sharp.peak_at + 20.0);
	// A broad one builds slowly from light, through moderate, to heavy: no jump
	assert(broad.rise_s > 70.0);
	assert(broad.biggest_step < 0.025f);
	// A sharp one is a wall of rain: quick to climb, short, with a gust ahead of it
	assert(sharp.rise_s < 40.0);
	assert(sharp.biggest_step > broad.biggest_step * 2.0f);
	assert(sharp.gone_s - sharp.rain_at < broad.gone_s - broad.rain_at);
	assert(sharp.wind_peak > broad.wind_peak + 0.6f && sharp.wind_peak > 2.8f);
	// The wind picks up ahead of the rain's peak, and falls away behind it
	assert(broad.wind_peak_at < broad.peak_at + 1.0 && sharp.wind_peak_at < sharp.peak_at + 15.0);
	assert(broad.wind_peak > 1.7f);
}

// How much of an hour, over a lot of places, it rains where it falls from cells
static float rainy_share(const WeatherDayData* day, int snow_wanted, float* cloud_when_rain) {
	weather_cells_input in = input_for(day);
	int rainy = 0;
	int total = 0;
	int shut = 0;

	in.use_cells = 1;
	in.centre_x = 0.0f;
	in.centre_y = 0.0f;
	in.day_start_s = in.shared_s - 600.0;
	in.day_length_s = 7200.0;
	for(int t = 0; t < 3000; t += 20)
		for(int k = 0; k < 8; k++) {
			weather_cell_sample s;
			float fall;

			in.shared_s = 24000000.0 + (double)t;
			weather_cells_sample(&in, -4000.0f + (float)k * 1100.0f, (float)k * 330.0f, &s);
			fall = snow_wanted ? s.snow : s.rain;
			total++;
			if(fall > 0.1f) {
				rainy++;
				shut += s.cloud > 0.7f;
			}
		}
	if(cloud_when_rain)
		*cloud_when_rain = rainy ? (float)shut / (float)rainy : 1.0f;

	return (float)rainy / (float)total;
}

static void test_a_rainy_day_is_made_of_cells_that_come_and_go(void) {
	WeatherDayData light = day_of(WEATHER_RAIN, 1, 1);
	WeatherDayData moderate = day_of(WEATHER_RAIN, 2, 1);
	WeatherDayData heavy = day_of(WEATHER_RAIN, 3, 1);
	WeatherDayData cloudy = day_of(WEATHER_CLOUDY, 0, 1);
	WeatherDayData snow = day_of(WEATHER_SNOW, 2, 1);
	float under_cloud;
	float a = rainy_share(&light, 0, NULL);
	float b = rainy_share(&moderate, 0, &under_cloud);
	float c = rainy_share(&heavy, 0, NULL);

	// It rains part of the time, more on a heavier day, and it is dry in between
	assert(a > 0.10f && a < 0.70f);
	assert(b > a && c > b);
	assert(c < 0.97f);
	// Rain is only ever under thick cloud
	assert(under_cloud > 0.9f);
	assert(rainy_share(&cloudy, 0, NULL) == 0.0f);
	// A snow day falls as snow, and not rain
	assert(rainy_share(&snow, 1, NULL) > 0.2f && rainy_share(&snow, 0, NULL) == 0.0f);
	// And the same everywhere the same time: nothing here is rolled at random
	assert(rainy_share(&moderate, 0, NULL) == b);
}

static void test_a_scripted_storm_builds_and_fades_smoothly(void) {
	weather_envelope e;
	float peak = 0.0f;
	float last_rain = -1.0f;
	float last_cloud = -1.0f;
	float biggest_step = 0.0f;
	double length = 480.0;
	int peak_at = 0;

	for(int t = 0; t < 480; t++) {
		assert(weather_storm_envelope((double)t, length, 0, &e) == 1);
		if(last_rain >= 0.0f) {
			float step = fabsf(e.rain - last_rain);

			if(fabsf(e.cloud - last_cloud) > step)
				step = fabsf(e.cloud - last_cloud);
			if(step > biggest_step)
				biggest_step = step;
		}
		if(e.rain > peak) {
			peak = e.rain;
			peak_at = t;
		}
		if(t == 0)
			assert(e.rain < 0.01f && e.cloud < 0.1f && e.storm == 0.0f);
		assert(e.storm == 0.0f && e.coverage == 1.0f);
		assert(e.wind >= 1.0f && e.wind <= 2.3f);
		last_rain = e.rain;
		last_cloud = e.cloud;
	}
	// Clear to heavy and back, with no jump: the most the rain or the cloud moves in a second
	assert(peak > 0.9f && peak_at > 200 && peak_at < 320);
	assert(biggest_step < 0.02f);
	// It passes through light, moderate and heavy on the way up and again on the way down
	for(int side = 0; side < 2; side++) {
		int light = 0;
		int moderate = 0;
		int heavy = 0;

		for(int t = side ? 300 : 0; t < (side ? 480 : 300); t++) {
			weather_storm_envelope((double)t, length, 0, &e);
			light |= fabsf(e.rain - 0.28f) < 0.02f;
			moderate |= fabsf(e.rain - 0.55f) < 0.02f;
			heavy |= fabsf(e.rain - 0.92f) < 0.02f;
		}
		assert(light && moderate);
		if(!side)
			assert(heavy);
	}
	// Over, it is clear and says so
	assert(weather_storm_envelope(480.0, length, 0, &e) == 0);
	assert(e.rain == 0.0f && e.cloud < 0.1f);
	// Thunder only at the heaviest
	weather_storm_envelope(0.0, length, 1, &e);
	assert(e.storm == 0.0f);
	weather_storm_envelope(length * 0.30, length, 1, &e);
	assert(e.storm == 0.0f);
	weather_storm_envelope(length * 0.60, length, 1, &e);
	assert(e.storm > 0.5f);
	weather_storm_envelope(length * 0.90, length, 1, &e);
	assert(e.storm == 0.0f);
}

int main(void) {
	test_samples_repeat_and_stay_in_range();
	test_a_heavier_day_wets_more_of_the_ground();
	test_a_heavier_day_falls_harder();
	test_a_spot_sees_the_rain_come_and_go();
	test_a_heavy_day_is_wet_nearly_all_the_time();
	test_the_patches_drift_along_the_wind();
	test_the_pattern_is_not_sensitive_to_how_late_the_clock_is();
	test_only_the_right_kind_of_day_makes_each_fall();
	test_a_dry_cloudy_day_has_thick_and_thin_places();
	test_without_a_known_yesterday_the_new_weather_is_everywhere();
	test_a_new_day_rolls_in_as_a_front_along_the_wind();
	test_behind_the_front_the_old_weather_is_gone();
	test_the_cloud_of_a_front_comes_before_its_rain();
	test_cloud_spreads_wider_than_rain();
	test_a_storm_can_be_seen_from_where_it_is_not_overhead();
	test_the_grid_is_the_samples();
	test_winds_get_faster_by_level();
	test_a_scripted_storm_builds_and_fades_smoothly();
	test_a_called_storm_comes_in_goes_over_and_passes();
	test_a_rainy_day_is_made_of_cells_that_come_and_go();
	printf("weather_cells_test: all passed\n");

	return 0;
}
