#ifndef WEATHER_STATE_H
#define WEATHER_STATE_H

// The forecast the server sent, kept between its packets. No GL.

#include "protocol.h"

#include <stddef.h>
#include <stdint.h>

// A forecast packet with its numbers in the host's byte order
typedef struct {
	uint32_t day_index;
	uint32_t day_length_ms;
	uint32_t ms_to_next_day;
	int region_count;
	WeatherDayData days[WEATHER_REGION_MAX][WEATHER_FORECAST_DAYS];
} WeatherForecast;

// Forget the forecast, as at the end of a session
void weather_state_reset(void);

// Keep a forecast that arrived at now_s; nonzero when kept, 0 when refused
int weather_state_apply(const WeatherForecast* forecast, double now_s);

// Whether a forecast has arrived this session
int weather_state_synced(void);

// How many regions the forecast covers; 0 before it arrives
int weather_state_region_count(void);

// How many weather days have begun since the forecast arrived
int weather_state_days_elapsed(double now_s);

// A region's weather ahead days from now; nonzero when the table reaches it
int weather_state_day(int region, int ahead, double now_s, WeatherDayData* out);

// Real milliseconds until the next weather day; 0 before a forecast arrives
uint32_t weather_state_ms_to_next_day(double now_s);

// Keep the frame's time, so the readers below need no clock passed in
void weather_state_tick(double now_s);

// Seconds on the clock every client shares: the server's weather days counted
// out in seconds. The local clock until a forecast arrives.
double weather_state_shared_s(double now_s);

// The same, at the time last given to weather_state_tick
double weather_state_shared_now_s(void);

// Seconds since today began on the shared clock; 0 before a forecast arrives
double weather_state_into_day_s(double now_s);
double weather_state_into_day_now_s(void);

// The day before today, when this client has seen it: nonzero when known. It is
// what a day's weather replaces as a front rolls in.
int weather_state_prev_day(int region, double now_s, WeatherDayData* out);
int weather_state_prev_day_now(int region, WeatherDayData* out);

// The weather days this client knows, by number on the shared clock (the shared
// seconds divided by the day length). The table reaches ahead from today, and
// back one day to the one this client saw end, if it did.

// Seconds in a weather day; 0 before a forecast arrives
double weather_state_day_length_s(void);

// The earliest day known; 0 before a forecast arrives
int64_t weather_state_first_day(void);

// A region's weather on a numbered day; nonzero when that day is known
int weather_state_day_by_index(int region, int64_t day, WeatherDayData* out);

// The same two, at the time last given to weather_state_tick
int weather_state_day_now(int region, int ahead, WeatherDayData* out);
uint32_t weather_state_ms_to_next_day_now(void);

// The region a world position (pixels) is in: the nearest capital
int weather_region_at(float x, float y);

// A region's name, or "" when there is no such region
const char* weather_region_name(int region);

// "Calm", "Breezy", "Windy" or "Gale" for a wind level 0 to 3
const char* weather_wind_name(int level);

// The speeds a wind level spans: "10-25 km/h", or in miles an hour
void weather_wind_range_text(int level, int metric, char* out, size_t size);

// A day's weather with how hard it falls: "Light Rain", "Heavy Snow", "Severe Thunderstorm"
void weather_day_name(const WeatherDayData* day, char* out, size_t size);

// The same, shorter, to fit a small place: "Light Rain", "Severe Storm"
void weather_day_short_name(const WeatherDayData* day, char* out, size_t size);

// A condition's name, or "" when there is no such condition
const char* weather_condition_name(int condition);

// "Today", "Tomorrow", "In 3 days"
void weather_day_label(int ahead, char* out, size_t size);

// The temperature at an hour (0 to 24): coldest at 3 am, warmest at 3 pm
int weather_temp_at_hour(const WeatherDayData* day, float hour);

// Whether an hour (0 to 24) is before 6 am or from 6 pm, as the sun sets
int weather_is_night(float hour);

// A Celsius reading in the unit wanted
int weather_temp_convert(int celsius, int to_celsius);

// A reading with its unit letter, such as "57 F" or "-3 C"
void weather_temp_text(int celsius, int use_celsius, char* out, size_t size);

#endif /* WEATHER_STATE_H */
