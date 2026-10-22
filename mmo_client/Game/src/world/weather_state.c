// The weather forecast, kept from the server's word between its packets.

#include "world/weather_state.h"
#include "world_regions.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int g_synced;
static double g_base_s;
static double g_tick_s;
static WeatherForecast g_forecast;
// The day before the forecast's today, as far as this client has seen it
static WeatherDayData g_prev[WEATHER_REGION_MAX];
static int g_prev_valid[WEATHER_REGION_MAX];
static uint32_t g_prev_for_day;

static const char* const CONDITION_NAMES[WEATHER_CONDITION_COUNT] = {
	"Clear", "Partly Cloudy", "Cloudy", "Rain", "Thunderstorm", "Snow"
};

void weather_state_reset(void) {
	g_synced = 0;
	g_base_s = 0.0;
	g_tick_s = 0.0;
	memset(&g_forecast, 0, sizeof(g_forecast));
	memset(g_prev, 0, sizeof(g_prev));
	memset(g_prev_valid, 0, sizeof(g_prev_valid));
	g_prev_for_day = 0;
}

int weather_state_apply(const WeatherForecast* forecast, double now_s) {
	if(forecast->region_count <= 0 || forecast->region_count > WEATHER_REGION_MAX)
		return 0;
	if(forecast->day_length_ms == 0 || forecast->ms_to_next_day > forecast->day_length_ms)
		return 0;

	// A new day arriving keeps the one it replaces, so the old weather can be
	// seen giving way to the new
	if(g_synced && forecast->day_index > g_forecast.day_index) {
		int64_t slot = (int64_t)forecast->day_index - 1 - (int64_t)g_forecast.day_index;

		memset(g_prev_valid, 0, sizeof(g_prev_valid));
		if(slot >= 0 && slot < WEATHER_FORECAST_DAYS) {
			for(int r = 0; r < g_forecast.region_count; r++) {
				g_prev[r] = g_forecast.days[r][slot];
				g_prev_valid[r] = 1;
			}
		}
		g_prev_for_day = forecast->day_index;
	} else if(!g_synced || forecast->day_index < g_forecast.day_index) {
		memset(g_prev_valid, 0, sizeof(g_prev_valid));
	}

	g_forecast = *forecast;
	g_base_s = now_s;
	g_synced = 1;

	return 1;
}

int weather_state_synced(void) {
	return g_synced;
}

int weather_state_region_count(void) {
	return g_synced ? g_forecast.region_count : 0;
}

// Milliseconds since the forecast arrived; a clock that ran backwards is none
static int64_t elapsed_ms(double now_s) {
	double since = (now_s - g_base_s) * 1000.0;

	return since > 0.0 ? (int64_t)(since + 0.5) : 0;
}

int weather_state_days_elapsed(double now_s) {
	int64_t since = elapsed_ms(now_s);
	int64_t days;

	if(!g_synced || since < (int64_t)g_forecast.ms_to_next_day)
		return 0;

	days = 1 + (since - g_forecast.ms_to_next_day) / g_forecast.day_length_ms;

	return days > WEATHER_FORECAST_DAYS ? WEATHER_FORECAST_DAYS : (int)days;
}

int weather_state_day(int region, int ahead, double now_s, WeatherDayData* out) {
	int slot;

	if(!g_synced || region < 0 || region >= g_forecast.region_count)
		return 0;
	if(ahead < 0 || ahead >= WEATHER_FORECAST_DAYS)
		return 0;

	slot = weather_state_days_elapsed(now_s) + ahead;
	if(slot >= WEATHER_FORECAST_DAYS)
		return 0;

	*out = g_forecast.days[region][slot];

	return 1;
}

uint32_t weather_state_ms_to_next_day(double now_s) {
	int64_t since = elapsed_ms(now_s);

	if(!g_synced)
		return 0;
	if(since < (int64_t)g_forecast.ms_to_next_day)
		return (uint32_t)(g_forecast.ms_to_next_day - since);

	since -= g_forecast.ms_to_next_day;

	return g_forecast.day_length_ms - (uint32_t)(since % g_forecast.day_length_ms);
}

void weather_state_tick(double now_s) {
	g_tick_s = now_s;
}

double weather_state_shared_s(double now_s) {
	int64_t since;
	int64_t day;
	int64_t into;
	int64_t length;

	if(!g_synced)
		return now_s;

	since = elapsed_ms(now_s);
	length = (int64_t)g_forecast.day_length_ms;
	if(since < (int64_t)g_forecast.ms_to_next_day) {
		day = (int64_t)g_forecast.day_index;
		into = length - (int64_t)g_forecast.ms_to_next_day + since;
	} else {
		since -= (int64_t)g_forecast.ms_to_next_day;
		day = (int64_t)g_forecast.day_index + 1 + since / length;
		into = since % length;
	}

	return (double)(day * length + into) / 1000.0;
}

double weather_state_into_day_s(double now_s) {
	int64_t since;
	int64_t length;

	if(!g_synced)
		return 0.0;
	since = elapsed_ms(now_s);
	length = (int64_t)g_forecast.day_length_ms;
	if(since < (int64_t)g_forecast.ms_to_next_day)
		return (double)(length - (int64_t)g_forecast.ms_to_next_day + since) / 1000.0;

	return (double)((since - (int64_t)g_forecast.ms_to_next_day) % length) / 1000.0;
}

double weather_state_into_day_now_s(void) {
	return weather_state_into_day_s(g_tick_s);
}

int weather_state_prev_day(int region, double now_s, WeatherDayData* out) {
	int elapsed;

	if(!g_synced || region < 0 || region >= g_forecast.region_count)
		return 0;
	elapsed = weather_state_days_elapsed(now_s);
	if(elapsed >= 1) {
		if(elapsed > WEATHER_FORECAST_DAYS)
			return 0;
		*out = g_forecast.days[region][elapsed - 1];

		return 1;
	}
	if(!g_prev_valid[region] || g_prev_for_day != g_forecast.day_index)
		return 0;
	*out = g_prev[region];

	return 1;
}

double weather_state_day_length_s(void) {
	return g_synced ? (double)g_forecast.day_length_ms / 1000.0 : 0.0;
}

int64_t weather_state_first_day(void) {
	if(!g_synced)
		return 0;

	for(int r = 0; r < g_forecast.region_count; r++)
		if(!g_prev_valid[r] || g_prev_for_day != g_forecast.day_index)
			return (int64_t)g_forecast.day_index;

	return (int64_t)g_forecast.day_index - 1;
}

int weather_state_day_by_index(int region, int64_t day, WeatherDayData* out) {
	int64_t slot;

	if(!g_synced || region < 0 || region >= g_forecast.region_count)
		return 0;

	slot = day - (int64_t)g_forecast.day_index;
	if(slot >= 0 && slot < WEATHER_FORECAST_DAYS) {
		*out = g_forecast.days[region][slot];

		return 1;
	}
	if(slot == -1 && g_prev_valid[region] && g_prev_for_day == g_forecast.day_index) {
		*out = g_prev[region];

		return 1;
	}

	return 0;
}

int weather_state_prev_day_now(int region, WeatherDayData* out) {
	return weather_state_prev_day(region, g_tick_s, out);
}

double weather_state_shared_now_s(void) {
	return weather_state_shared_s(g_tick_s);
}

int weather_state_day_now(int region, int ahead, WeatherDayData* out) {
	return weather_state_day(region, ahead, g_tick_s, out);
}

uint32_t weather_state_ms_to_next_day_now(void) {
	return weather_state_ms_to_next_day(g_tick_s);
}

int weather_region_at(float x, float y) {
	int best = 0;
	float best_distance = -1.0f;

	for(int i = 0; i < WORLD_CITY_COUNT; i++) {
		float dx = x - (float)WORLD_CITIES[i].tile_x * WORLD_TILE_PX;
		float dy = y - (float)WORLD_CITIES[i].tile_y * WORLD_TILE_PX;
		float distance = dx * dx + dy * dy;

		if(best_distance < 0.0f || distance < best_distance) {
			best = i;
			best_distance = distance;
		}
	}

	return best;
}

const char* weather_region_name(int region) {
	if(region < 0 || region >= WORLD_CITY_COUNT)
		return "";
	return WORLD_CITIES[region].city_name;
}

const char* weather_wind_name(int level) {
	static const char* const NAMES[4] = { "Calm", "Breezy", "Windy", "Gale" };

	if(level < 0)
		level = 0;
	if(level > 3)
		level = 3;

	return NAMES[level];
}

void weather_wind_range_text(int level, int metric, char* out, size_t size) {
	// Kilometres an hour, and the same in miles an hour
	static const char* const KMH[4] = { "0-10", "10-25", "25-50", "50+" };
	static const char* const MPH[4] = { "0-6", "6-15", "15-30", "30+" };

	if(level < 0)
		level = 0;
	if(level > 3)
		level = 3;
	snprintf(out, size, "%s %s", metric ? KMH[level] : MPH[level], metric ? "km/h" : "mph");
}

void weather_day_name(const WeatherDayData* day, char* out, size_t size) {
	static const char* const WORDS[4] = { "", "Light ", "", "Heavy " };
	int level = day->intensity > 3 ? 3 : day->intensity;
	const char* base = weather_condition_name(day->condition);

	// A thunderstorm is a storm already; only a heavy one is called severe
	if(day->condition == WEATHER_THUNDER) {
		snprintf(out, size, "%s%s", level >= 3 ? "Severe " : "", base);
		return;
	}
	if(day->condition == WEATHER_RAIN || day->condition == WEATHER_SNOW) {
		snprintf(out, size, "%s%s", WORDS[level], base);
		return;
	}
	snprintf(out, size, "%s", base);
}

void weather_day_short_name(const WeatherDayData* day, char* out, size_t size) {
	static const char* const WORDS[4] = { "", "Light ", "", "Heavy " };
	int level = day->intensity > 3 ? 3 : day->intensity;

	if(day->condition == WEATHER_THUNDER) {
		snprintf(out, size, "%sStorm", level >= 3 ? "Severe " : "");
		return;
	}
	if(day->condition == WEATHER_RAIN || day->condition == WEATHER_SNOW) {
		snprintf(out, size, "%s%s", WORDS[level], weather_condition_name(day->condition));
		return;
	}
	snprintf(out, size, "%s", weather_condition_name(day->condition));
}

const char* weather_condition_name(int condition) {
	if(condition < 0 || condition >= WEATHER_CONDITION_COUNT)
		return "";
	return CONDITION_NAMES[condition];
}

void weather_day_label(int ahead, char* out, size_t size) {
	if(ahead <= 0)
		snprintf(out, size, "Today");
	else if(ahead == 1)
		snprintf(out, size, "Tomorrow");
	else
		snprintf(out, size, "In %d days", ahead);
}

int weather_temp_at_hour(const WeatherDayData* day, float hour) {
	float warmth = 0.5f + 0.5f * cosf(2.0f * 3.14159265f * (hour - 15.0f) / 24.0f);

	return (int)lroundf((float)day->temp_lo + (float)(day->temp_hi - day->temp_lo) * warmth);
}

int weather_is_night(float hour) {
	return hour < 6.0f || hour >= 18.0f;
}

int weather_temp_convert(int celsius, int to_celsius) {
	if(to_celsius)
		return celsius;
	return (int)lroundf((float)celsius * 1.8f + 32.0f);
}

void weather_temp_text(int celsius, int use_celsius, char* out, size_t size) {
	snprintf(out, size, "%d %c", weather_temp_convert(celsius, use_celsius),
	         use_celsius ? 'C' : 'F');
}
