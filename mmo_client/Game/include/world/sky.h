#ifndef WORLD_SKY_H
#define WORLD_SKY_H

// The colours and lights of the sky at an hour of the day. No GL.
// Directions are GL world space: +X east, +Y up, +Z south. Colours are 0..1.

#include "world/weather_now.h"

typedef struct {
	float zenith[3];    // straight up
	float horizon[3];   // the sky just above the horizon line
	float haze[3];      // the ground below the horizon line, far haze
	float sun_dir[3];   // toward the sun, unit length
	float sun_color[3];
	float moon_dir[3];  // toward the moon, unit length
	float star_amount;  // 0 by day, 1 on a dark night
} sky_state;

// The sky at an hour, 0 up to 24. Other hours wrap round the day.
void sky_at(float hour, sky_state* out);

// Grey and darken a sky under cloud and storm, and light it for lightning
void sky_apply_weather(sky_state* sky, const weather_now* weather);

// Set the sky the renderers draw: the hour's sky under this moment's weather
void sky_update(float hour);

// The sky last given to sky_update; noon until then
const sky_state* sky_current(void);

#endif /* WORLD_SKY_H */
