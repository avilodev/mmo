#ifndef MAP_WEATHER_H
#define MAP_WEATHER_H

// The storm cover over a map: today's weather where the map is looking.

#include "ui/map_layout.h"
#include "world/weather_state.h"

// What a cover's texture was painted for, so it is painted again only on a change
typedef struct {
	int valid;
	map_projection proj;
	map_rect area;
	WeatherDayData days[WEATHER_REGION_MAX];
	int count;
} map_weather_painted;

// One cover and its texture; zero it before first use. Each place that shows a
// cover owns one, so showing one does not repaint another.
typedef struct {
	unsigned int texture;
	map_weather_painted painted;
} map_weather_cover;

// Paint the weather over the map area; nothing before a forecast
void map_weather_cover_draw(map_weather_cover* cover, const map_rect* area,
                            const map_projection* proj);

// Free the cover's texture
void map_weather_cover_release(map_weather_cover* cover);

// The full map's own cover, inside its clip
void map_weather_draw(const map_rect* area, const map_projection* proj);

// Free the full map's texture
void map_weather_release(void);

#endif /* MAP_WEATHER_H */
