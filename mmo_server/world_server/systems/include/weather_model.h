#ifndef WEATHER_MODEL_H
#define WEATHER_MODEL_H

// The weather forecast: a table of seven days a region, made by hashed dice

#include "protocol.h"
#include "world_regions.h"

#include <stdint.h>

// Real seconds in one weather day: two hours
#define WEATHER_DEFAULT_DAY_S 7200

// Real seconds a weather day may be set to by the config, at least and at most
#define WEATHER_DAY_S_MIN 60
#define WEATHER_DAY_S_MAX 604800

// The seed used when the config gives none
#define WEATHER_DEFAULT_SEED 0x5EEDFACE2026ULL

// Take the config values; 0 keeps the default of each. Run once at startup.
void weather_configure(int64_t epoch_s, int day_length_s, uint64_t seed);

// The epoch, day length and seed in use
int64_t weather_epoch_s(void);
uint32_t weather_day_length_ms(void);
uint64_t weather_seed(void);

// The weather day a real time falls in; days before the epoch are negative
int64_t weather_day_index(int64_t unix_ms, int64_t epoch_s, uint32_t day_length_ms);

// Real milliseconds from a real time until the next weather day begins
uint32_t weather_ms_to_next_day(int64_t unix_ms, int64_t epoch_s, uint32_t day_length_ms);

// A region's forecast for a day as it stood lead days (0 to 6) before it
WeatherDayData weather_forecast_day(uint64_t seed, int region, int64_t day, int lead);

// Fill in the packet that tells a player the forecast at a real time
void weather_build_packet(WeatherForecastPacket* pkt, uint32_t character_id, int64_t unix_ms);

#endif /* WEATHER_MODEL_H */
