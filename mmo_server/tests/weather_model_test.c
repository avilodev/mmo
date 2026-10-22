// Check the weather forecast: a formula of seed and day, and its packet

#include "weather_model.h"
#include "protocol.h"
#include "world_regions.h"

#include <arpa/inet.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define EPOCH_S 1767225600LL
#define DAY_MS 7200000u
#define SEED 0x1234ABCDULL

// Regions in WORLD_CITIES order
#define ENNARA 0
#define KINGDOM_1 1
#define KINGDOM_2 2
#define KINGDOM_3 3

static int same_day(WeatherDayData a, WeatherDayData b) {
	return memcmp(&a, &b, sizeof(a)) == 0;
}

// The sky apart from temperature: rain and snow count as one
static int sky_of(int condition) {
	if(condition == WEATHER_RAIN || condition == WEATHER_SNOW)
		return 3;
	if(condition == WEATHER_THUNDER)
		return 4;
	return condition;
}

static void test_day_zero_is_the_epoch(void) {
	assert(weather_day_index(EPOCH_S * 1000, EPOCH_S, DAY_MS) == 0);
	assert(weather_day_index(EPOCH_S * 1000 + DAY_MS - 1, EPOCH_S, DAY_MS) == 0);
	assert(weather_day_index(EPOCH_S * 1000 + DAY_MS, EPOCH_S, DAY_MS) == 1);
	assert(weather_day_index(EPOCH_S * 1000 + 5 * DAY_MS + 1, EPOCH_S, DAY_MS) == 5);
}

static void test_days_before_the_epoch_count_down(void) {
	assert(weather_day_index(EPOCH_S * 1000 - 1, EPOCH_S, DAY_MS) == -1);
	assert(weather_day_index(EPOCH_S * 1000 - DAY_MS, EPOCH_S, DAY_MS) == -1);
	assert(weather_day_index(EPOCH_S * 1000 - DAY_MS - 1, EPOCH_S, DAY_MS) == -2);
}

static void test_a_zero_day_length_is_day_zero_not_a_crash(void) {
	assert(weather_day_index(EPOCH_S * 1000 + 99, EPOCH_S, 0) == 0);
	assert(weather_ms_to_next_day(EPOCH_S * 1000 + 99, EPOCH_S, 0) == 0);
}

static void test_the_time_to_the_next_day_counts_down(void) {
	assert(weather_ms_to_next_day(EPOCH_S * 1000, EPOCH_S, DAY_MS) == DAY_MS);
	assert(weather_ms_to_next_day(EPOCH_S * 1000 + 1, EPOCH_S, DAY_MS) == DAY_MS - 1);
	assert(weather_ms_to_next_day(EPOCH_S * 1000 + DAY_MS - 1, EPOCH_S, DAY_MS) == 1);
	assert(weather_ms_to_next_day(EPOCH_S * 1000 - 1, EPOCH_S, DAY_MS) == 1);
}

static void test_the_same_seed_gives_the_same_forecast(void) {
	for(int region = 0; region < WORLD_CITY_COUNT; region++) {
		for(int lead = 0; lead < WEATHER_FORECAST_DAYS; lead++) {
			WeatherDayData first = weather_forecast_day(SEED, region, 400, lead);
			WeatherDayData again = weather_forecast_day(SEED, region, 400, lead);

			assert(same_day(first, again));
		}
	}
}

static void test_another_seed_gives_other_weather(void) {
	int differ = 0;

	for(int day = 0; day < 200; day++)
		if(!same_day(weather_forecast_day(SEED, ENNARA, day, 0),
		             weather_forecast_day(SEED + 1, ENNARA, day, 0)))
			differ++;
	assert(differ > 100);
}

static void test_every_field_is_in_range(void) {
	for(int region = 0; region < WORLD_CITY_COUNT; region++)
		for(int day = -50; day < 1500; day++)
			for(int lead = 0; lead < WEATHER_FORECAST_DAYS; lead++) {
				WeatherDayData d = weather_forecast_day(SEED, region, day, lead);

				assert(d.condition < WEATHER_CONDITION_COUNT);
				// A forecast is never quite certain, even for the day itself
				assert(d.precip_pct <= 99);
				assert(d.temp_lo < d.temp_hi);
				assert(d.revised < WEATHER_REVISED_COUNT);
			}
}

static void test_snow_only_where_it_is_cold(void) {
	int snow = 0;
	int rain = 0;

	for(int region = 0; region < WORLD_CITY_COUNT; region++)
		for(int day = 0; day < 3000; day++) {
			WeatherDayData d = weather_forecast_day(SEED, region, day, 3);

			if(d.condition == WEATHER_SNOW) {
				assert(d.temp_hi <= 1);
				snow++;
			}
			if(d.condition == WEATHER_RAIN || d.condition == WEATHER_THUNDER) {
				assert(d.temp_hi > 1);
				rain++;
			}
		}
	// Both happen somewhere in the world, so the check above ran on real cases
	assert(snow > 50);
	assert(rain > 50);
}

static void test_a_lead_out_of_range_is_clamped_not_a_crash(void) {
	assert(same_day(weather_forecast_day(SEED, ENNARA, 10, 99),
	                weather_forecast_day(SEED, ENNARA, 10, WEATHER_FORECAST_DAYS - 1)));
	assert(same_day(weather_forecast_day(SEED, ENNARA, 10, -4),
	                weather_forecast_day(SEED, ENNARA, 10, 0)));
}

static void test_a_region_out_of_range_is_clamped_not_a_crash(void) {
	WeatherDayData d = weather_forecast_day(SEED, 99, 10, 0);

	assert(d.condition < WEATHER_CONDITION_COUNT);
}

// A forecast made a week out rarely changes, but it does change
static void test_a_week_old_forecast_is_mostly_right(void) {
	int samples = 0;
	int wrong = 0;

	for(int region = 0; region < WORLD_CITY_COUNT; region++)
		for(int day = 0; day < 3000; day++) {
			WeatherDayData far = weather_forecast_day(SEED, region, day, 6);
			WeatherDayData near = weather_forecast_day(SEED, region, day, 0);

			samples++;
			if(sky_of(far.condition) != sky_of(near.condition))
				wrong++;
		}
	// Six revision chances of 2-4 percent each add up to about 17 percent
	assert(wrong * 100 > samples * 10);
	assert(wrong * 100 < samples * 24);
}

// One day closer, a day changes only when a storm moved
static void test_a_day_closer_changes_only_when_flagged(void) {
	int flagged = 0;

	for(int region = 0; region < WORLD_CITY_COUNT; region++)
		for(int day = 0; day < 2000; day++)
			for(int lead = 1; lead < WEATHER_FORECAST_DAYS; lead++) {
				WeatherDayData before = weather_forecast_day(SEED, region, day, lead);
				WeatherDayData after = weather_forecast_day(SEED, region, day, lead - 1);

				if(sky_of(before.condition) != sky_of(after.condition)) {
					assert(after.revised != WEATHER_REVISED_NONE);
					flagged++;
				}
			}
	assert(flagged > 100);
}

static void test_a_revision_is_remembered_until_the_day(void) {
	for(int day = 0; day < 2000; day++) {
		int seen = 0;

		for(int lead = WEATHER_FORECAST_DAYS - 1; lead >= 0; lead--) {
			WeatherDayData d = weather_forecast_day(SEED, ENNARA, day, lead);

			if(seen)
				assert(d.revised != WEATHER_REVISED_NONE);
			if(d.revised != WEATHER_REVISED_NONE)
				seen = 1;
		}
	}
}

static int is_wet(int condition) {
	return condition == WEATHER_RAIN || condition == WEATHER_THUNDER || condition == WEATHER_SNOW;
}

static void test_levels_are_in_range_and_fall_only_where_it_falls(void) {
	for(int region = 0; region < WORLD_CITY_COUNT; region++)
		for(int day = -50; day < 1500; day++)
			for(int lead = 0; lead < WEATHER_FORECAST_DAYS; lead++) {
				WeatherDayData d = weather_forecast_day(SEED, region, day, lead);

				assert(d.wind <= 3);
				assert(d.intensity <= 3);
				// A dry day has nothing to be light or heavy; a wet one is at least light
				if(is_wet(d.condition))
					assert(d.intensity >= 1);
				else
					assert(d.intensity == 0);
				// A thunderstorm is never light
				if(d.condition == WEATHER_THUNDER)
					assert(d.intensity >= 2);
			}
}

static void test_every_level_of_fall_and_wind_happens(void) {
	int fall[4] = { 0 };
	int wind[4] = { 0 };

	for(int region = 0; region < WORLD_CITY_COUNT; region++)
		for(int day = 0; day < 3000; day++) {
			WeatherDayData d = weather_forecast_day(SEED, region, day, 0);

			fall[d.intensity]++;
			wind[d.wind]++;
		}
	for(int level = 0; level < 4; level++) {
		assert(fall[level] > 100);
		assert(wind[level] > 50);
	}
	// Light falls are commoner than heavy ones; calm days commoner than gales
	assert(fall[1] > fall[3] * 3 / 4);
	assert(wind[0] > wind[3]);
}

static void test_storms_blow_harder_than_clear_days(void) {
	long storm_wind = 0;
	long storms = 0;
	long clear_wind = 0;
	long clears = 0;

	for(int region = 0; region < WORLD_CITY_COUNT; region++)
		for(int day = 0; day < 3000; day++) {
			WeatherDayData d = weather_forecast_day(SEED, region, day, 0);

			if(d.condition == WEATHER_THUNDER) {
				storm_wind += d.wind;
				storms++;
			} else if(d.condition == WEATHER_CLEAR) {
				clear_wind += d.wind;
				clears++;
			}
		}
	assert(storms > 50 && clears > 50);
	assert(storm_wind * clears > clear_wind * storms * 2);
}

// A forecast made far ahead is mostly right about how hard it will be, and
// sometimes a step off
static void test_a_far_forecast_is_a_step_off_now_and_then(void) {
	int same = 0;
	int off_one = 0;
	int worse = 0;

	for(int region = 0; region < WORLD_CITY_COUNT; region++)
		for(int day = 0; day < 3000; day++) {
			WeatherDayData far = weather_forecast_day(SEED, region, day, 6);
			WeatherDayData near = weather_forecast_day(SEED, region, day, 0);
			int gap = (int)far.wind - (int)near.wind;

			if(sky_of(far.condition) != sky_of(near.condition))
				continue;
			if(gap == 0)
				same++;
			else if(gap == 1 || gap == -1)
				off_one++;
			else
				worse++;
		}
	assert(same > off_one * 2);
	assert(off_one > 100);
	assert(worse < off_one / 4);
}

static void test_the_fall_and_wind_of_a_day_do_not_depend_on_the_seed_of_the_lead(void) {
	// Two looks at the same day on the same lead agree
	for(int day = 0; day < 500; day++) {
		WeatherDayData a = weather_forecast_day(SEED, KINGDOM_2, day, 3);
		WeatherDayData b = weather_forecast_day(SEED, KINGDOM_2, day, 3);

		assert(a.intensity == b.intensity && a.wind == b.wind);
	}
}

static void test_chance_of_rain_follows_the_sky(void) {
	int wet = 0;
	int dry = 0;
	int wet_n = 0;
	int dry_n = 0;

	for(int day = 0; day < 3000; day++) {
		WeatherDayData d = weather_forecast_day(SEED, ENNARA, day, 2);

		if(d.condition == WEATHER_RAIN || d.condition == WEATHER_SNOW ||
		   d.condition == WEATHER_THUNDER) {
			wet += d.precip_pct;
			wet_n++;
		} else if(d.condition == WEATHER_CLEAR) {
			dry += d.precip_pct;
			dry_n++;
		}
	}
	assert(wet_n > 100 && dry_n > 100);
	assert(wet / wet_n > 60);
	assert(dry / dry_n < 15);
}

static void test_the_cold_kingdom_is_colder_than_the_warm_one(void) {
	long cold = 0;
	long warm = 0;

	for(int day = 0; day < 2000; day++) {
		cold += weather_forecast_day(SEED, KINGDOM_2, day, 0).temp_hi;
		warm += weather_forecast_day(SEED, KINGDOM_3, day, 0).temp_hi;
	}
	assert(warm / 2000 - cold / 2000 >= 15);
}

// Kingdom 2 is upwind of Kingdom 1, so yesterday's sky there shows up here
static long matches_yesterday(int region, int upwind) {
	long hits = 0;

	for(int day = 1; day < 3000; day++) {
		int here = sky_of(weather_forecast_day(SEED, region, day, 6).condition);
		int there = sky_of(weather_forecast_day(SEED, upwind, day - 1, 6).condition);

		if(here == there)
			hits++;
	}
	return hits;
}

static void test_a_front_travels_downwind(void) {
	long linked = matches_yesterday(KINGDOM_1, KINGDOM_2);
	long unlinked = matches_yesterday(KINGDOM_1, KINGDOM_3);

	assert(linked > unlinked + 3000 / 8);
}

static void test_the_packet_carries_the_table_in_network_order(void) {
	WeatherForecastPacket pkt;
	int64_t now_ms = EPOCH_S * 1000 + 5 * (int64_t)DAY_MS + 1000;
	int64_t today;

	weather_configure(EPOCH_S, 7200, SEED);
	today = weather_day_index(now_ms, EPOCH_S, DAY_MS);
	weather_build_packet(&pkt, 77, now_ms);

	assert(pkt.header.type == PACKET_WEATHER_FORECAST);
	assert(ntohl(pkt.header.player_id) == 77);
	assert(ntohs(pkt.header.payload_size) == sizeof(pkt) - sizeof(PacketHeader));
	assert(today == 5);
	assert(ntohl(pkt.day_index) == 5);
	assert(ntohl(pkt.day_length_ms) == DAY_MS);
	assert(ntohl(pkt.ms_to_next_day) == DAY_MS - 1000);
	assert(pkt.region_count == WORLD_CITY_COUNT);

	for(int region = 0; region < WORLD_CITY_COUNT; region++)
		for(int d = 0; d < WEATHER_FORECAST_DAYS; d++)
			assert(same_day(pkt.days[region][d], weather_forecast_day(SEED, region, today + d, d)));
}

static void test_unused_regions_in_the_packet_are_zero(void) {
	WeatherForecastPacket pkt;
	WeatherDayData zero;

	memset(&zero, 0, sizeof(zero));
	weather_configure(EPOCH_S, 7200, SEED);
	weather_build_packet(&pkt, 1, EPOCH_S * 1000);

	for(int region = WORLD_CITY_COUNT; region < WEATHER_REGION_MAX; region++)
		for(int d = 0; d < WEATHER_FORECAST_DAYS; d++)
			assert(same_day(pkt.days[region][d], zero));
}

static void test_the_next_day_slides_the_table_one_place(void) {
	WeatherForecastPacket first;
	WeatherForecastPacket next;

	weather_configure(EPOCH_S, 7200, SEED);
	weather_build_packet(&first, 1, EPOCH_S * 1000);
	weather_build_packet(&next, 1, (EPOCH_S + 7200) * 1000);

	// A day one place nearer only changes sky when it was flagged as moved
	for(int d = 1; d < WEATHER_FORECAST_DAYS; d++) {
		WeatherDayData old = first.days[ENNARA][d];
		WeatherDayData now = next.days[ENNARA][d - 1];

		if(sky_of(old.condition) != sky_of(now.condition))
			assert(now.revised != WEATHER_REVISED_NONE);
	}
}

static void test_configuring_twice_changes_nothing(void) {
	WeatherForecastPacket a;
	WeatherForecastPacket b;

	weather_configure(EPOCH_S, 7200, SEED);
	weather_build_packet(&a, 1, (EPOCH_S + 99999) * 1000);
	weather_configure(EPOCH_S, 7200, SEED);
	weather_build_packet(&b, 1, (EPOCH_S + 99999) * 1000);
	assert(memcmp(&a, &b, sizeof(a)) == 0);
}

static void test_zero_config_values_keep_the_defaults(void) {
	weather_configure(0, 0, 0);
	assert(weather_day_length_ms() == WEATHER_DEFAULT_DAY_S * 1000u);
	assert(weather_seed() == WEATHER_DEFAULT_SEED);
	assert(weather_epoch_s() != 0);
}

int main(void) {
	test_day_zero_is_the_epoch();
	test_days_before_the_epoch_count_down();
	test_a_zero_day_length_is_day_zero_not_a_crash();
	test_the_time_to_the_next_day_counts_down();
	test_the_same_seed_gives_the_same_forecast();
	test_another_seed_gives_other_weather();
	test_every_field_is_in_range();
	test_levels_are_in_range_and_fall_only_where_it_falls();
	test_every_level_of_fall_and_wind_happens();
	test_storms_blow_harder_than_clear_days();
	test_a_far_forecast_is_a_step_off_now_and_then();
	test_the_fall_and_wind_of_a_day_do_not_depend_on_the_seed_of_the_lead();
	test_snow_only_where_it_is_cold();
	test_a_lead_out_of_range_is_clamped_not_a_crash();
	test_a_region_out_of_range_is_clamped_not_a_crash();
	test_a_week_old_forecast_is_mostly_right();
	test_a_day_closer_changes_only_when_flagged();
	test_a_revision_is_remembered_until_the_day();
	test_chance_of_rain_follows_the_sky();
	test_the_cold_kingdom_is_colder_than_the_warm_one();
	test_a_front_travels_downwind();
	test_the_packet_carries_the_table_in_network_order();
	test_unused_regions_in_the_packet_are_zero();
	test_the_next_day_slides_the_table_one_place();
	test_configuring_twice_changes_nothing();
	test_zero_config_values_keep_the_defaults();
	printf("weather_model_test: all passed\n");
	return 0;
}
