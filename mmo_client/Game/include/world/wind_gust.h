#ifndef WIND_GUST_H
#define WIND_GUST_H

// Bands of stronger wind sweeping over the city; hair and ribbons share them

#include "world/weather_now.h"

// A gust is two waves: length and speed, world units (whole lengths per wrap)
#define WIND_GUST_LENGTH_A 640.0
#define WIND_GUST_SPEED_A 250.0
#define WIND_GUST_LENGTH_B 384.0
#define WIND_GUST_SPEED_B 300.0

// Hair's lean at wind level 1 as a share of bone length; grows with level^2
#define WIND_LEAN_PER_LEVEL_SQ 0.03f

// The lean in a lull, and how much more a full gust adds
#define WIND_LEAN_LULL 0.4f
#define WIND_LEAN_GUST_SPAN 0.9f

// The lift hair gets, as a share of its lean
#define WIND_LEAN_LIFT 0.25f

// The way the wind blows: a unit vector over world (x, y)
void wind_direction(const weather_now* weather, float out[2]);

// The gust at world (x, y) at a shared time: 0 a lull, 1 a full gust
float wind_gust_at(const float dir[2], float time_s, float x, float y);

// The lean of hair at world (x, y), a share of bone length, axes (x, up, z)
void wind_lean_at(const weather_now* weather, float x, float y, float out[3]);

// GLSL for wind_gust(vec2 xz, vec2 dir, float t), the same bands
const char* wind_gust_glsl(void);

#endif /* WIND_GUST_H */
