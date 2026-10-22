#ifndef WEATHER_CELLS_H
#define WEATHER_CELLS_H

// The weather over the ground round the player: patches of cloud and rain and
// snow that drift along the prevailing wind, so it comes and goes, and a new
// day's weather that rolls in as a front. Everything is a formula of the
// forecast, the shared weather clock and a world position, so every player
// sees the same weather in the same place. No GL.

#include "protocol.h"

// What a day's weather means over a whole region, before it is broken into patches
typedef struct {
	float cloud; // how covered the sky is under it, 0..1
	float rain; // how hard the rain falls at the core of a patch, 0..1
	float snow; // the same for snow
	float storm; // how stormy the core of a patch is, 0..1
	float coverage; // the share of the ground a fall reaches at any moment, 0..1
	float wind; // the wind level, 0 calm to 3 gale
} weather_envelope;

// What the weather is at one spot, each 0 to 1 except the wind level
typedef struct {
	float cloud;
	float rain;
	float snow;
	float storm;
	float wind; // 0 to 3
	float fresh; // how much of this is the new day's weather: 0 old, 1 new
} weather_cell_sample;

// A storm cell: a cloud that is made already and travels along the wind, over
// the ground, so it rains as it comes, hardest at its middle, and passes.
// A broad one builds slowly and trails off; a sharp one is a narrow wall of
// hard rain with a gust ahead of it.
typedef struct {
	double arrive_s; // the shared clock when its middle is over the region's middle
	float cross; // how far its track runs off that middle, across the way it goes
	float sigma; // how far ahead of its middle the rain reaches, world units
	float peak; // how hard it rains in its middle, 0..1
	float sharp; // 0 broad and gradual, 1 short and hard
	float speed; // world units a second
	int snow; // falls as snow
	int thunder; // has lightning
} weather_cell;

// What goes into a sample
typedef struct {
	weather_envelope today;
	weather_envelope before; // the day today replaces
	int has_before; // 0 when it is not known: today's weather is everywhere at once
	double into_day_s; // seconds since today began
	double shared_s; // the shared weather clock
	float centre_x; // the middle of the region, world units
	float centre_y;
	// Rain and snow fall from cells that come through, not from drifting patches
	int use_cells;
	double day_start_s; // the shared clock when today began, for cells
	double day_length_s;
	weather_envelope after; // tomorrow, for cells that arrive after today ends
	int has_after;
	// One storm called up by hand, besides whatever the day brings
	int extra_on;
	weather_cell extra;
} weather_cells_input;

// The envelope of a forecast day
void weather_envelope_of(const WeatherDayData* day, weather_envelope* out);

// A storm called up by hand whose middle goes over (x, y) at hits_s on the shared
// clock. sharp is 0 for a broad one, 1 for a hard one.
void weather_cell_called(const weather_cells_input* in, int sharp, int thunder, double hits_s,
                         float x, float y, weather_cell* out);

// How long a called storm takes to be over after its middle has passed, seconds
double weather_cell_after_s(const weather_cell* cell);

// A scripted storm: clear, then light, moderate and heavy rain, then back down
// to clear, smoothly. Fills the whole region's weather `elapsed` seconds into a
// storm that lasts `length`; thunder adds lightning at the peak. Returns 0 once
// the storm is over (and fills a clear day).
int weather_storm_envelope(double elapsed, double length, int thunder, weather_envelope* out);

// How far a scripted storm's front has got, in the seconds of a day's front
// (weather_cells_input.into_day_s): the cloud comes in from upwind, and the
// front is at the region's middle when the rain begins
double weather_storm_front_s(double elapsed, double length);

// The weather at a world position
void weather_cells_sample(const weather_cells_input* in, float x, float y,
                          weather_cell_sample* out);

// Fill a square grid of count by count samples, cell world units apart, the
// first at (x0, y0). Row by row.
void weather_cells_grid(const weather_cells_input* in, float x0, float y0, float cell, int count,
                        weather_cell_sample* out);

// The stormiest the weather is within radius of a spot: where lightning can be
// seen from, though it is not overhead
float weather_cells_storm_near(const weather_cells_input* in, float x, float y, float radius);

// The way the weather travels, a unit vector in world (x, y), and how fast, units a second
void weather_cells_drift(float* dir_x, float* dir_y, float* speed);

// How far along the wind a front has got, a world distance from the region's
// middle, after into_day_s seconds of the day. Negative is still upwind of it.
float weather_cells_front_at(double into_day_s);

// A wind level 0 to 4 (it may be between; storms push it past a gale) as a speed,
// world units a second
float weather_wind_speed(float level);

#endif /* WEATHER_CELLS_H */
