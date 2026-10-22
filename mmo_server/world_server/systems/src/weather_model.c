// The weather forecast, a formula of the world seed and the day.

#include "weather_model.h"
#include "world_clock.h"

#include <arpa/inet.h>
#include <string.h>

typedef enum {
	SKY_CLEAR = 0,
	SKY_PARTLY,
	SKY_CLOUDY,
	SKY_WET,
	SKY_STORM,
	SKY_COUNT
} Sky;

// What a region's weather tends to be
typedef struct {
	int weight[SKY_COUNT]; // chance of each sky out of 100, for a fresh draw
	int mean_hi; // the usual daytime high, Celsius
	int anomaly; // how far a spell of days strays from it, up or down
	int upwind; // the region weather arrives from, or -1
} Climate;

// In WORLD_CITIES order; placeholders until the world bible says otherwise
static const Climate g_climates[] = {
	{ { 30, 22, 20, 21, 7 }, 14, 6, 1 },
	{ { 24, 20, 24, 24, 8 }, 6, 6, 2 },
	{ { 18, 18, 26, 30, 8 }, -2, 6, -1 },
	{ { 50, 22, 14, 11, 3 }, 26, 5, 2 }
};

#define CLIMATE_COUNT ((int)(sizeof(g_climates) / sizeof(g_climates[0])))

// A fresh draw is not needed every day: a sky tends to stay the same
#define STAY_PERCENT 45

// How often a region's new sky is simply what blew in from upwind yesterday
#define FRONT_PERCENT 60

// Days of history the sky chain looks back through before it forgets
#define CHAIN_DEPTH 8

// The surest a forecast ever is of rain or snow, percent
#define PRECIP_MAX 99

// Highest temperature at which a wet day falls as snow
#define SNOW_MAX_HI 1

// Chance in a thousand that a day changes in the step from lead k to k - 1
static const int g_revise_per_mille[WEATHER_FORECAST_DAYS] = { 0, 20, 20, 30, 30, 40, 40 };

// How much warmer or colder each sky makes the day
static const int g_sky_offset[SKY_COUNT] = { 2, 1, 0, -2, -3 };

// How far the night falls below the day under each sky
static const int g_sky_swing[SKY_COUNT] = { 9, 7, 5, 4, 4 };

// Chance of rain or snow at lead 6, and how much it firms up per day nearer
static const int g_precip_base[SKY_COUNT] = { 8, 15, 30, 60, 75 };
static const int g_precip_slope[SKY_COUNT] = { -1, -2, -3, 5, 4 };

typedef enum {
	SALT_FRESH = 1,
	SALT_STAY,
	SALT_FRONT,
	SALT_ANOMALY,
	SALT_REVISE,
	SALT_REVISE_WAY,
	SALT_REVISE_FLAVOUR,
	SALT_REVISE_TEMP,
	SALT_PRECIP,
	SALT_LEVEL,
	SALT_LEVEL_SHIFT,
	SALT_WIND,
	SALT_WIND_SHIFT
} Salt;

// How hard a wet day falls, out of 100 for light, moderate and heavy: rain, a
// thunderstorm, snow, and the snow of a storm
static const int g_fall_weights[4][3] = {
	{ 40, 40, 20 },
	{ 0, 35, 65 },
	{ 35, 40, 25 },
	{ 10, 40, 50 }
};

// How hard the wind blows, out of 100 for calm, breezy, windy and gale, under
// each sky
static const int g_wind_weights[SKY_COUNT][4] = {
	{ 55, 35, 9, 1 },
	{ 40, 40, 17, 3 },
	{ 35, 40, 20, 5 },
	{ 20, 40, 30, 10 },
	{ 5, 20, 45, 30 }
};

// Chance in a thousand, for each day of lead, that the level of a fall or of
// the wind is forecast a step off
#define LEVEL_UNSURE_PER_MILLE 40

static int64_t g_epoch_s = WORLD_CLOCK_DEFAULT_EPOCH_S;
static uint32_t g_day_length_ms = WEATHER_DEFAULT_DAY_S * 1000u;
static uint64_t g_seed = WEATHER_DEFAULT_SEED;

void weather_configure(int64_t epoch_s, int day_length_s, uint64_t seed) {
	uint32_t day_s = day_length_s > 0 ? (uint32_t)day_length_s : WEATHER_DEFAULT_DAY_S;

	if(day_s > WEATHER_DAY_S_MAX)
		day_s = WEATHER_DAY_S_MAX;

	g_epoch_s = epoch_s != 0 ? epoch_s : WORLD_CLOCK_DEFAULT_EPOCH_S;
	g_day_length_ms = day_s * 1000u;
	g_seed = seed != 0 ? seed : WEATHER_DEFAULT_SEED;
}

int64_t weather_epoch_s(void) {
	return g_epoch_s;
}

uint32_t weather_day_length_ms(void) {
	return g_day_length_ms;
}

uint64_t weather_seed(void) {
	return g_seed;
}

int64_t weather_day_index(int64_t unix_ms, int64_t epoch_s, uint32_t day_length_ms) {
	int64_t since = unix_ms - epoch_s * 1000;
	int64_t day;

	if(day_length_ms == 0)
		return 0;
	day = since / (int64_t)day_length_ms;
	if(since % (int64_t)day_length_ms < 0)
		day--;

	return day;
}

uint32_t weather_ms_to_next_day(int64_t unix_ms, int64_t epoch_s, uint32_t day_length_ms) {
	int64_t since = unix_ms - epoch_s * 1000;
	int64_t into;

	if(day_length_ms == 0)
		return 0;
	into = since % (int64_t)day_length_ms;
	if(into < 0)
		into += day_length_ms;

	return (uint32_t)(day_length_ms - into);
}

// A well-mixed 64-bit value from a 64-bit one
static uint64_t mix(uint64_t x) {
	x += 0x9E3779B97F4A7C15ULL;
	x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
	x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;

	return x ^ (x >> 31);
}

// The same dice for the same seed, region, day, purpose and step, always
static uint64_t roll(uint64_t seed, int region, int64_t day, Salt salt, int step) {
	uint64_t h = mix(seed ^ 0xD1B54A32D192ED03ULL);

	h = mix(h ^ (uint64_t)region);
	h = mix(h ^ (uint64_t)day);
	h = mix(h ^ ((uint64_t)salt << 8) ^ (uint64_t)step);

	return h;
}

static const Climate* climate_of(int region) {
	if(region < 0 || region >= CLIMATE_COUNT)
		return &g_climates[0];
	return &g_climates[region];
}

// Pick a sky from the region's weights
static Sky fresh_sky(uint64_t seed, int region, int64_t day) {
	const Climate* climate = climate_of(region);
	int pick = (int)(roll(seed, region, day, SALT_FRESH, 0) % 100);
	int total = 0;

	for(int sky = 0; sky < SKY_COUNT; sky++) {
		total += climate->weight[sky];
		if(pick < total)
			return (Sky)sky;
	}

	return SKY_CLOUDY;
}

// The sky a day starts out with a week ahead, before any change of mind
static Sky first_sky(uint64_t seed, int region, int64_t day, int depth) {
	const Climate* climate = climate_of(region);

	if(depth <= 0)
		return fresh_sky(seed, region, day);
	if(climate->upwind >= 0 &&
	   (int)(roll(seed, region, day, SALT_FRONT, 0) % 100) < FRONT_PERCENT)
		return first_sky(seed, climate->upwind, day - 1, depth - 1);
	if((int)(roll(seed, region, day, SALT_STAY, 0) % 100) < STAY_PERCENT)
		return first_sky(seed, region, day - 1, depth - 1);

	return fresh_sky(seed, region, day);
}

// A spell of warm or cold days: three days' worth of dice, so it drifts
static int anomaly_of(uint64_t seed, int region, int64_t day) {
	const Climate* climate = climate_of(region);
	int span = climate->anomaly * 2 + 1;
	int sum = 0;

	for(int back = 0; back < 3; back++)
		sum += (int)(roll(seed, region, day - back, SALT_ANOMALY, 0) % (uint64_t)span) - climate->anomaly;

	return sum / 3;
}

// The sky one step away: a storm eases to rain, clear clouds over
static Sky neighbour_sky(Sky sky, uint64_t pick) {
	switch(sky) {
		case SKY_CLEAR:
			return SKY_PARTLY;
		case SKY_PARTLY:
			return pick % 2 == 0 ? SKY_CLEAR : SKY_CLOUDY;
		case SKY_CLOUDY:
			return pick % 2 == 0 ? SKY_PARTLY : SKY_WET;
		case SKY_WET:
			return pick % 2 == 0 ? SKY_CLOUDY : SKY_STORM;
		default:
			return SKY_WET;
	}
}

static int clamp_int(int value, int low, int high) {
	if(value < low)
		return low;
	return value > high ? high : value;
}

static uint8_t condition_of(Sky sky, int cold) {
	switch(sky) {
		case SKY_CLEAR:
			return WEATHER_CLEAR;
		case SKY_PARTLY:
			return WEATHER_PARTLY_CLOUDY;
		case SKY_CLOUDY:
			return WEATHER_CLOUDY;
		case SKY_WET:
			return cold ? WEATHER_SNOW : WEATHER_RAIN;
		default:
			return cold ? WEATHER_SNOW : WEATHER_THUNDER;
	}
}

// A day as it stands at one point of being forecast
typedef struct {
	Sky sky;
	int hi;
	int lo;
	int revised;
} DayState;

// The day as first guessed a week out, before any change of mind
static DayState first_guess(uint64_t seed, int region, int64_t day) {
	const Climate* climate = climate_of(region);
	DayState state;

	state.sky = first_sky(seed, region, day, CHAIN_DEPTH);
	state.hi = climate->mean_hi + anomaly_of(seed, region, day) + g_sky_offset[state.sky];
	state.lo = state.hi - g_sky_swing[state.sky];
	state.revised = WEATHER_REVISED_NONE;

	return state;
}

// The day after one more look: now and then the storm has gone elsewhere
static DayState look_again(DayState before, int cold, uint64_t seed, int region, int64_t day,
		int step) {
	DayState after = before;
	int nudge = (int)(roll(seed, region, day, SALT_REVISE_TEMP, step) % 3) - 1;

	if((int)(roll(seed, region, day, SALT_REVISE, step) % 1000) < g_revise_per_mille[step]) {
		Sky moved = neighbour_sky(before.sky, roll(seed, region, day, SALT_REVISE_WAY, step));
		int warmer = g_sky_offset[moved] - g_sky_offset[before.sky];

		after.sky = moved;
		after.hi += warmer;
		after.lo += warmer + g_sky_swing[before.sky] - g_sky_swing[moved];
		// A move that changes nothing on the icon is not worth a note
		if(condition_of(moved, cold) != condition_of(before.sky, cold)) {
			after.revised = WEATHER_REVISED_WEST +
			                (int)(roll(seed, region, day, SALT_REVISE_FLAVOUR, step) % 4);
		}
	}
	after.hi += nudge;
	after.lo += nudge;

	return after;
}

// Whether a wet day is snow: fixed by the spell's cold, not by later looks
static int is_cold_spell(uint64_t seed, int region, int64_t day) {
	const Climate* climate = climate_of(region);

	return climate->mean_hi + anomaly_of(seed, region, day) + g_sky_offset[SKY_WET] <= SNOW_MAX_HI;
}

// The chance of rain or snow: surer the nearer the day, and never certain
static int precip_chance(Sky sky, uint64_t seed, int region, int64_t day, int lead) {
	int chance = g_precip_base[sky] + g_precip_slope[sky] * (WEATHER_FORECAST_DAYS - 1 - lead);

	chance += (int)(roll(seed, region, day, SALT_PRECIP, lead) % 11) - 5;

	return clamp_int(chance, 0, PRECIP_MAX);
}

// A step off, up or down, now and then, the further ahead the forecast was made
static int unsure_level(uint64_t seed, int region, int64_t day, Salt salt, int lead, int level,
		int lowest, int highest) {
	int way;

	if((int)(roll(seed, region, day, salt, lead) % 1000) >= LEVEL_UNSURE_PER_MILLE * lead)
		return level;
	way = (roll(seed, region, day, salt, lead + 16) % 2) == 0 ? -1 : 1;

	return clamp_int(level + way, lowest, highest);
}

// How hard a day's rain or snow falls, 1 light to 3 heavy; 0 when nothing falls
static int fall_level(uint64_t seed, int region, int64_t day, uint8_t condition, Sky sky, int lead) {
	int kind;
	int pick;
	int total = 0;
	int level = 1;

	if(condition != WEATHER_RAIN && condition != WEATHER_THUNDER && condition != WEATHER_SNOW)
		return 0;
	if(condition == WEATHER_RAIN)
		kind = 0;
	else if(condition == WEATHER_THUNDER)
		kind = 1;
	else
		kind = sky == SKY_STORM ? 3 : 2;
	pick = (int)(roll(seed, region, day, SALT_LEVEL, 0) % 100);
	for(int l = 0; l < 3; l++) {
		total += g_fall_weights[kind][l];
		if(pick < total) {
			level = l + 1;
			break;
		}
	}

	return unsure_level(seed, region, day, SALT_LEVEL_SHIFT, lead, level, condition == WEATHER_THUNDER ? 2 : 1, 3);
}

// How hard the wind blows on a day, 0 calm to 3 gale
static int wind_level(uint64_t seed, int region, int64_t day, Sky sky, int lead) {
	int pick = (int)(roll(seed, region, day, SALT_WIND, 0) % 100);
	int total = 0;
	int level = 3;

	for(int l = 0; l < 4; l++) {
		total += g_wind_weights[sky][l];
		if(pick < total) {
			level = l;
			break;
		}
	}

	return unsure_level(seed, region, day, SALT_WIND_SHIFT, lead, level, 0, 3);
}

WeatherDayData weather_forecast_day(uint64_t seed, int region, int64_t day, int lead) {
	DayState state = first_guess(seed, region, day);
	int cold = is_cold_spell(seed, region, day);
	WeatherDayData out;

	lead = clamp_int(lead, 0, WEATHER_FORECAST_DAYS - 1);
	for(int step = WEATHER_FORECAST_DAYS - 1; step > lead; step--)
		state = look_again(state, cold, seed, region, day, step);

	out.condition = condition_of(state.sky, cold);
	if(out.condition == WEATHER_SNOW && state.hi > SNOW_MAX_HI)
		state.hi = SNOW_MAX_HI;
	else if(out.condition != WEATHER_SNOW && state.hi <= SNOW_MAX_HI && state.sky >= SKY_WET)
		state.hi = SNOW_MAX_HI + 1;
	if(state.lo >= state.hi)
		state.lo = state.hi - 1;

	out.precip_pct = (uint8_t)precip_chance(state.sky, seed, region, day, lead);
	out.temp_hi = (int8_t)clamp_int(state.hi, -100, 100);
	out.temp_lo = (int8_t)clamp_int(state.lo, -100, 100);
	out.revised = (uint8_t)state.revised;
	out.intensity = (uint8_t)fall_level(seed, region, day, out.condition, state.sky, lead);
	out.wind = (uint8_t)wind_level(seed, region, day, state.sky, lead);
	memset(out._padding, 0, sizeof(out._padding));

	return out;
}

void weather_build_packet(WeatherForecastPacket* pkt, uint32_t character_id, int64_t unix_ms) {
	int64_t today = weather_day_index(unix_ms, g_epoch_s, g_day_length_ms);

	memset(pkt, 0, sizeof(*pkt));
	pkt->header.type = PACKET_WEATHER_FORECAST;
	pkt->header.player_id = htonl(character_id);
	pkt->header.payload_size = htons(sizeof(*pkt) - sizeof(PacketHeader));
	pkt->day_index = htonl((uint32_t)today);
	pkt->day_length_ms = htonl(g_day_length_ms);
	pkt->ms_to_next_day = htonl(weather_ms_to_next_day(unix_ms, g_epoch_s, g_day_length_ms));
	pkt->region_count = WORLD_CITY_COUNT < WEATHER_REGION_MAX ? WORLD_CITY_COUNT : WEATHER_REGION_MAX;

	for(int region = 0; region < pkt->region_count; region++)
		for(int d = 0; d < WEATHER_FORECAST_DAYS; d++)
			pkt->days[region][d] = weather_forecast_day(g_seed, region, today + d, d);
}
