#ifndef WEATHER_FIELD_H
#define WEATHER_FIELD_H

// The weather over the whole map, blended from each region's forecast. No GL.

#include "protocol.h"

// What the weather is like at one place, each 0 to 1 except the temperature
typedef struct {
	float cloud;
	float rain;
	float storm;
	float snow;
	float temp_c;
} weather_sample;

typedef enum {
	WEATHER_LAYER_CLOUDS,
	WEATHER_LAYER_TEMPERATURE,
	WEATHER_LAYER_COUNT
} weather_layer;

// The regions' weather blended at a world position (pixels)
void weather_field_blend(const WeatherDayData* regions, int region_count, float x, float y,
                         weather_sample* out);

// Smooth noise from 0 to 1 at a world position; the seed picks the pattern
float weather_field_noise(float x, float y, unsigned int seed);

// Break a sample's cover into patches so storms look like storms, not gradients
void weather_field_roughen(weather_sample* sample, float x, float y, unsigned int seed);

// The colour a layer paints a sample, as red, green, blue and opacity (0 to 1)
void weather_field_color(weather_layer layer, const weather_sample* sample, float rgba[4]);

#endif /* WEATHER_FIELD_H */
