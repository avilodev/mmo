#ifndef WEATHER_NOW_H
#define WEATHER_NOW_H

// The weather as it is this moment where the player stands: how cloudy, how
// hard it falls, how wet the ground is. It is the forecast's weather broken
// into patches that drift past (world/weather_cells.h), eased a little, with
// lightning from the shared weather clock, so everyone in the same place sees
// the same thing. No GL.

#include "protocol.h"
#include "world/weather_cells.h"

typedef struct {
	float cloud; // 0 clear sky, 1 solid overcast
	float rain; // 0 none, 1 downpour
	float snow; // 0 none, 1 blizzard
	float storm; // 0 no thunder, 1 thunderstorm
	float storm_near; // the stormiest it is within sight, 0..1
	float wet; // 0 dry ground, 1 soaked
	float snow_cover; // 0 bare ground, 1 white all over
	float fog; // fog density a world unit here, 0 none
	float wind[2]; // the sideways push of the fall, world units a second (x, z)
	float wind_level; // 0 calm, 1 breezy, 2 windy, 3 gale (between, too)
	float wind_travel[2]; // how far the wind has carried things, wrapped (x, z)
	float flash; // 0..1 flash of lightning on the sky and the light
	float bolt; // 0..1 how bright the bolt in the sky is
	float bolt_angle; // the compass way of the bolt, radians
	float bolt_seed; // differs from strike to strike, 0..1
	float time_s; // shared seconds, wrapped, for things that move quickly
	float cloud_time_s; // shared seconds, wrapped far out, for the slow cloud drift
} weather_now;

// What wind_travel is wrapped at, world units: a loose thing carried by the wind
// comes back to where it was after this far, which suits a repeating lattice
#define WEATHER_TRAVEL_WRAP 2200.0

// Clear weather, dry ground, as at the start of a session
void weather_now_reset(void);

// Follow the forecast for a region, with the player standing at world (x, y).
// dt is the frame's seconds.
void weather_now_update(float dt, int region, float x, float y);

// The weather last given to weather_now_update; clear until then
const weather_now* weather_now_get(void);

// What the weather over the ground is made from this frame: the day, the day
// before, how far into the day, the clock. A map of the patches is built from it.
const weather_cells_input* weather_now_cells(void);

// What it is like overhead right now, in a few words: "Light rain", "Dry for
// now", "Overcast", "Clear". The forecast says the day; this says the minute.
void weather_now_describe(char* out, size_t size);

// The weather's amount of fog where a spot has this weather
float weather_fog_density(float cloud, float rain, float snow, float storm);

// The lightning at a shared time. storm_near is how stormy it is within sight
// (strikes happen while it is), and storm_here how stormy overhead (how bright
// the flash is). Fills flash, bolt, bolt_angle and bolt_seed, and leaves the rest alone.
void weather_now_lightning(double shared_s, float storm_near, float storm_here, weather_now* out);

// Make the weather a day of this condition, moderate and breezy, everywhere at
// once whatever the forecast says (for /weather); -1 gives the forecast back
void weather_now_force(int condition);

// The same for any day of weather: how hard it falls and how hard it blows
// too. With roll set it arrives as a front, rolling in over the clear sky that
// was there; without, it is just there.
void weather_now_force_day(const WeatherDayData* day, int roll);

// Run a scripted storm on this computer, over `seconds`: clear, then light,
// moderate and heavy rain, then back down to clear, all smooth. With thunder it
// has lightning at its peak. It ends on a clear day that /weather off undoes.
void weather_now_start_storm(double seconds, int thunder);

// A storm that is made already and travels to you: a cloud you see coming, that
// rains on you, hardest at its middle, and passes. sharp is 0 for a broad one that
// builds gradually and trails off, 1 for a short hard one with a gust ahead of it.
// It is the same for everyone in the same place. It ends on a clear day that
// /weather off undoes.
void weather_now_start_cell(int sharp, int thunder);

// 1 while a called storm is on its way or passing
int weather_now_cell_running(void);

// How far a scripted storm has got, or 0 when none is running
int weather_now_storm_progress(double* elapsed, double* length);

// The condition forced, or -1
int weather_now_forced(void);

// The whole forced day: nonzero when one is forced
int weather_now_forced_day(WeatherDayData* out);

#endif /* WEATHER_NOW_H */
