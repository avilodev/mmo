// The weather as it is this moment, eased from the patches over the ground.

#include "world/weather_now.h"
#include "world/weather_state.h"
#include "world_regions.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define PI_F 3.14159265f

// Seconds the shared clock is wrapped at, so floats on the GPU keep their detail
#define TIME_WRAP_S 16384.0

// The clouds drift slowly, so their clock wraps a good deal further out
#define CLOUD_WRAP_S 1048576.0

// Seconds an easing value takes to cover most of a change. The patches already
// move smoothly, so these only soften the edges of them.
#define CLOUD_EASE_S 8.0f
#define FALL_EASE_S 3.0f
#define WIND_EASE_S 6.0f

// Seconds of rain to soak the ground, and for it to dry; the same for snow cover
#define SOAK_S 90.0f
#define DRY_S 600.0f
#define SNOW_BUILD_S 240.0f
#define SNOW_MELT_S 900.0f
#define RAIN_WASH_S 200.0f

// Below this a fall is too light to wet the ground or build up snow
#define FALL_MIN 0.05f

// Joining, the ground is made as the last HISTORY_S seconds of weather left it,
// replayed in steps of HISTORY_STEP_S
#define HISTORY_S 1800.0
#define HISTORY_STEP_S 10.0

// Lightning is rolled once in each slot of this many seconds
#define STRIKE_SLOT_S 11.0
#define STRIKE_LENGTH_S 1.2

// How far from the player a storm still counts as in sight, world units
#define STORM_SIGHT 4500.0f

// How the wind swings about the course the weather travels, radians, and how
// strongly it gusts, as a share of its speed
#define WIND_SWING 0.45f
#define GUST_LOW 0.65f
#define GUST_SPAN 0.70f

typedef enum {
	SALT_WIND_WAY = 1,
	SALT_GUST,
	SALT_STRIKE,
	SALT_STRIKE_AT,
	SALT_STRIKE_WAY,
	SALT_STRIKE_SEED
} Salt;

static weather_now g_now;
static weather_cells_input g_cells;
static int g_primed;
static int g_forced = -1;
static WeatherDayData g_forced_day;
static int g_forced_roll;
// A scripted storm: running, when it began on the shared clock, how long, thunder
static int g_storm_on;
static int g_storm_began;
static double g_storm_at;
static double g_storm_length;
static int g_storm_thunder;
// A called storm: running, made yet, the made cell, and when its middle reaches the player
static int g_cell_on;
static int g_cell_sharp;
static int g_cell_thunder;
static int g_cell_made;
static weather_cell g_cell;
static double g_cell_hits;
// Where the player stood at the last update, for where a called storm goes
static float g_last_x;
static float g_last_y;

// A called storm's middle goes over the player this long after it is called, seconds:
// a broad one has a long way to come, a sharp one is on you sooner
#define CELL_LEAD_BROAD_S 300.0
#define CELL_LEAD_SHARP_S 150.0
static double g_forced_at;
static int g_forced_at_set;
// How far the wind has carried things, kept in double so it stays exact
static double g_travel[2];

static float clamp01(float x) {
	if(x < 0.0f)
		return 0.0f;
	if(x > 1.0f)
		return 1.0f;

	return x;
}

static float smooth_step(float lo, float hi, float x) {
	float t = clamp01((x - lo) / (hi - lo));

	return t * t * (3.0f - 2.0f * t);
}

static uint64_t mix64(uint64_t x) {
	x += 0x9E3779B97F4A7C15ULL;
	x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
	x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;

	return x ^ (x >> 31);
}

// A repeatable number from 0 up to 1 for a whole number and a purpose
static float hash01(int64_t n, Salt salt) {
	uint64_t h = mix64((uint64_t)n ^ ((uint64_t)salt << 56));

	return (float)(h >> 40) / 16777216.0f;
}

// Smooth noise from 0 to 1 that wanders over about one period
static float wander(double t, double period, Salt salt) {
	double x = t / period;
	double whole = floor(x);
	float f = (float)(x - whole);
	float a = hash01((int64_t)whole, salt);
	float b = hash01((int64_t)whole + 1, salt);

	f = f * f * (3.0f - 2.0f * f);

	return a + (b - a) * f;
}

static float approach(float now, float target, float dt, float seconds) {
	return now + (target - now) * (1.0f - expf(-dt / seconds));
}

float weather_fog_density(float cloud, float rain, float snow, float storm) {
	return smooth_step(0.5f, 1.0f, cloud) * 0.00006f + rain * 0.00022f + snow * 0.00040f +
	       storm * 0.00008f;
}

// A flash dies away fast
static float pulse(double since) {
	return since < 0.0 ? 0.0f : expf((float)(-since * 9.0));
}

void weather_now_lightning(double shared_s, float storm_near, float storm_here, weather_now* out) {
	int64_t slot = (int64_t)floor(shared_s / STRIKE_SLOT_S);
	double since;
	float flash;

	out->flash = 0.0f;
	out->bolt = 0.0f;
	out->bolt_angle = 0.0f;
	out->bolt_seed = 0.0f;
	if(storm_near < 0.3f || hash01(slot, SALT_STRIKE) > 0.7f * storm_near)
		return;

	since = shared_s - ((double)slot * STRIKE_SLOT_S +
	        (double)hash01(slot, SALT_STRIKE_AT) * (STRIKE_SLOT_S - 3.0));
	if(since < 0.0 || since > STRIKE_LENGTH_S)
		return;

	flash = pulse(since);
	if(pulse(since - 0.17) * 0.75f > flash)
		flash = pulse(since - 0.17) * 0.75f;
	if(pulse(since - 0.36) * 0.5f > flash)
		flash = pulse(since - 0.36) * 0.5f;
	// A storm far off lights the sky a little; one overhead lights everything
	out->flash = flash * (0.15f * storm_near + 0.85f * storm_here);
	out->bolt = since < 0.30 ? 1.0f - (float)since / 0.30f : 0.0f;
	out->bolt_angle = hash01(slot, SALT_STRIKE_WAY) * 2.0f * PI_F;
	out->bolt_seed = hash01(slot, SALT_STRIKE_SEED);
}

void weather_now_reset(void) {
	memset(&g_now, 0, sizeof(g_now));
	memset(&g_cells, 0, sizeof(g_cells));
	g_travel[0] = 0.0;
	g_travel[1] = 0.0;
	g_primed = 0;
	g_storm_on = 0;
	g_cell_on = 0;
}

// A ground wet or snowy after dt seconds of this much rain and snow
static void ground_step(float* wet, float* snow_cover, float rain, float snow, float dt) {
	if(rain > FALL_MIN)
		*wet += dt * rain / SOAK_S * 2.0f;
	else
		*wet -= dt / DRY_S;
	if(snow > FALL_MIN)
		*snow_cover += dt * snow / SNOW_BUILD_S * 2.0f;
	else
		*snow_cover -= dt / SNOW_MELT_S;
	if(rain > FALL_MIN)
		*snow_cover -= dt / RAIN_WASH_S;
	*wet = clamp01(*wet);
	*snow_cover = clamp01(*snow_cover);
}

// The wind and the ground wet or snowy after a frame
static void settle_ground(float dt) {
	ground_step(&g_now.wet, &g_now.snow_cover, g_now.rain, g_now.snow, dt);
}

// What goes into the patches at a past moment, from the days this client knows.
// A moment before the first day it knows is taken to be in that day.
static void history_cells(int region, double shared, double length,
		weather_cells_input* in) {
	int64_t first = weather_state_first_day();
	int64_t day = (int64_t)floor(shared / length);
	WeatherDayData data;
	WeatherDayData other;

	memset(in, 0, sizeof(*in));
	in->shared_s = shared;
	if(region >= 0 && region < WORLD_CITY_COUNT) {
		in->centre_x = (float)WORLD_CITIES[region].tile_x * WORLD_TILE_PX;
		in->centre_y = (float)WORLD_CITIES[region].tile_y * WORLD_TILE_PX;
	}
	if(day < first)
		day = first;
	if(!weather_state_day_by_index(region, day, &data))
		memset(&data, 0, sizeof(data));

	in->use_cells = 1;
	in->day_length_s = length;
	in->day_start_s = (double)day * length;
	in->into_day_s = shared - in->day_start_s;
	weather_envelope_of(&data, &in->today);
	if(weather_state_day_by_index(region, day + 1, &other)) {
		weather_envelope_of(&other, &in->after);
		in->has_after = 1;
	}
	if(weather_state_day_by_index(region, day - 1, &other)) {
		weather_envelope_of(&other, &in->before);
		in->has_before = 1;
	}
}

// The wet and snowy ground a player joining now finds: what the last half hour
// of the shared weather did at their spot, the same for everyone
static void seed_ground(int region, double shared, float x, float y) {
	double length = weather_state_day_length_s();
	float wet = 0.0f;
	float snow_cover = 0.0f;

	if(length <= 0.0)
		return;

	for(double at = shared - HISTORY_S; at < shared; at += HISTORY_STEP_S) {
		weather_cells_input in;
		weather_cell_sample then;

		history_cells(region, at, length, &in);
		weather_cells_sample(&in, x, y, &then);
		ground_step(&wet, &snow_cover, then.rain, then.snow, (float)HISTORY_STEP_S);
	}
	g_now.wet = wet;
	g_now.snow_cover = snow_cover;
}

// The scripted storm's weather this frame; when it is over, a clear day stays
static void run_storm(double shared) {
	if(!g_storm_began) {
		g_storm_at = shared;
		g_storm_began = 1;
	}
	if(weather_storm_envelope(shared - g_storm_at, g_storm_length, g_storm_thunder,
	                          &g_cells.today)) {
		// It comes in over a clear sky as a front from upwind, the cloud first
		WeatherDayData clear;

		memset(&clear, 0, sizeof(clear));
		clear.condition = WEATHER_CLEAR;
		clear.wind = 1;
		weather_envelope_of(&clear, &g_cells.before);
		g_cells.has_before = 1;
		g_cells.into_day_s = weather_storm_front_s(shared - g_storm_at, g_storm_length);
		return;
	}

	g_storm_on = 0;
	memset(&g_forced_day, 0, sizeof(g_forced_day));
	g_forced_day.condition = WEATHER_CLEAR;
	g_forced_day.wind = 1;
	g_forced = WEATHER_CLEAR;
	weather_envelope_of(&g_forced_day, &g_cells.today);
}

// The called storm this frame; when its tail has passed, a clear day stays
static void run_cell(double shared) {
	if(!g_cell_made) {
		g_cell_hits = shared + (g_cell_sharp ? CELL_LEAD_SHARP_S : CELL_LEAD_BROAD_S);
		weather_cell_called(&g_cells, g_cell_sharp, g_cell_thunder, g_cell_hits, g_last_x, g_last_y,
		                    &g_cell);
		g_cell_made = 1;
	}
	g_cells.extra_on = 1;
	g_cells.extra = g_cell;
	if(shared > g_cell_hits + weather_cell_after_s(&g_cell))
		g_cell_on = 0;
}

// What goes into the patches this frame: the forecast, or what was forced
static void gather_cells(int region, double shared) {
	WeatherDayData day;
	WeatherDayData before;
	int clear_day = 1;

	memset(&g_cells, 0, sizeof(g_cells));
	g_cells.shared_s = shared;
	if(region >= 0 && region < WORLD_CITY_COUNT) {
		g_cells.centre_x = (float)WORLD_CITIES[region].tile_x * WORLD_TILE_PX;
		g_cells.centre_y = (float)WORLD_CITIES[region].tile_y * WORLD_TILE_PX;
	}
	memset(&day, 0, sizeof(day));
	if(g_forced >= 0) {
		day = g_forced_day;
		clear_day = 0;
		if(!g_forced_at_set) {
			g_forced_at = shared;
			g_forced_at_set = 1;
		}
		if(g_forced_roll) {
			memset(&before, 0, sizeof(before));
			weather_envelope_of(&before, &g_cells.before);
			g_cells.has_before = 1;
			g_cells.into_day_s = shared - g_forced_at;
		}
	} else if(weather_state_day_now(region, 0, &day)) {
		clear_day = 0;
		g_cells.use_cells = 1;
		g_cells.day_length_s = weather_state_into_day_now_s() +
		                       (double)weather_state_ms_to_next_day_now() / 1000.0;
		g_cells.day_start_s = shared - weather_state_into_day_now_s();
		if(weather_state_day_now(region, 1, &before)) {
			weather_envelope_of(&before, &g_cells.after);
			g_cells.has_after = 1;
		}
		if(weather_state_prev_day_now(region, &before)) {
			weather_envelope_of(&before, &g_cells.before);
			g_cells.has_before = 1;
		}
		g_cells.into_day_s = weather_state_into_day_now_s();
	}
	if(clear_day)
		memset(&day, 0, sizeof(day));
	weather_envelope_of(&day, &g_cells.today);
	if(g_forced >= 0 && g_cell_on)
		run_cell(shared);
	if(g_forced >= 0 && g_storm_on) {
		run_storm(shared);
		return;
	}
	// A weather forced to be seen is everywhere, not in patches that may miss you
	if(g_forced >= 0 && !g_forced_roll && g_cells.today.coverage > 0.0f)
		g_cells.today.coverage = 1.0f;
}

void weather_now_update(float dt, int region, float x, float y) {
	double shared = weather_state_shared_now_s();
	weather_cell_sample here;
	float wind_dir_x;
	float wind_dir_y;
	float swing;
	float speed;
	float gust;

	if(dt < 0.0f)
		dt = 0.0f;
	g_last_x = x;
	g_last_y = y;
	gather_cells(region, shared);
	weather_cells_sample(&g_cells, x, y, &here);

	// The wind blows roughly along the way the weather travels, swinging a little
	weather_cells_drift(&wind_dir_x, &wind_dir_y, NULL);
	swing = (wander(shared, 600.0, SALT_WIND_WAY) - 0.5f) * 2.0f * WIND_SWING;
	gust = wander(shared, 25.0, SALT_GUST);
	speed = weather_wind_speed(here.wind) * (GUST_LOW + GUST_SPAN * gust);

	{
		float c = cosf(swing);
		float s = sinf(swing);
		float tx = (wind_dir_x * c - wind_dir_y * s) * speed;
		float ty = (wind_dir_x * s + wind_dir_y * c) * speed;

		if(!g_primed) {
			// Joining in the middle of the weather: it is already there, wind and all
			g_now.cloud = here.cloud;
			g_now.rain = here.rain;
			g_now.snow = here.snow;
			g_now.storm = here.storm;
			g_now.wind_level = here.wind;
			g_now.wind[0] = tx;
			g_now.wind[1] = ty;
			g_now.wet = here.rain > FALL_MIN ? 0.85f : 0.0f;
			g_now.snow_cover = here.snow > FALL_MIN ? 0.7f : 0.0f;
			// a forced weather has no past; the real one does
			if(g_forced < 0)
				seed_ground(region, shared, x, y);
			g_primed = 1;
		} else {
			g_now.cloud = approach(g_now.cloud, here.cloud, dt, CLOUD_EASE_S);
			g_now.rain = approach(g_now.rain, here.rain, dt, FALL_EASE_S);
			g_now.snow = approach(g_now.snow, here.snow, dt, FALL_EASE_S);
			g_now.storm = approach(g_now.storm, here.storm, dt, FALL_EASE_S);
			g_now.wind_level = approach(g_now.wind_level, here.wind, dt, WIND_EASE_S);
			g_now.wind[0] = approach(g_now.wind[0], tx, dt, WIND_EASE_S);
			g_now.wind[1] = approach(g_now.wind[1], ty, dt, WIND_EASE_S);
			settle_ground(dt);
		}
	}
	g_travel[0] = fmod(g_travel[0] + (double)g_now.wind[0] * (double)dt, WEATHER_TRAVEL_WRAP);
	g_travel[1] = fmod(g_travel[1] + (double)g_now.wind[1] * (double)dt, WEATHER_TRAVEL_WRAP);
	if(g_travel[0] < 0.0)
		g_travel[0] += WEATHER_TRAVEL_WRAP;
	if(g_travel[1] < 0.0)
		g_travel[1] += WEATHER_TRAVEL_WRAP;
	g_now.wind_travel[0] = (float)g_travel[0];
	g_now.wind_travel[1] = (float)g_travel[1];
	g_now.storm_near = weather_cells_storm_near(&g_cells, x, y, STORM_SIGHT);
	g_now.fog = weather_fog_density(g_now.cloud, g_now.rain, g_now.snow, g_now.storm);
	g_now.time_s = (float)fmod(shared, TIME_WRAP_S);
	g_now.cloud_time_s = (float)fmod(shared, CLOUD_WRAP_S);
	weather_now_lightning(shared, g_now.storm_near, g_now.storm, &g_now);
}

const weather_now* weather_now_get(void) {
	return &g_now;
}

const weather_cells_input* weather_now_cells(void) {
	return &g_cells;
}

void weather_now_describe(char* out, size_t size) {
	const weather_now* w = &g_now;
	const char* text;

	if(w->storm > 0.3f && w->rain > 0.1f)
		text = w->storm > 0.7f ? "Thunderstorm" : "Thunder and rain";
	else if(w->rain > 0.03f)
		text = w->rain < 0.2f ? "Drizzle" : (w->rain < 0.45f ? "Light rain" : (w->rain < 0.75f ? "Rain" : "Heavy rain"));
	else if(w->snow > 0.03f)
		text = w->snow < 0.2f ? "Flurries" : (w->snow < 0.45f ? "Light snow" : (w->snow < 0.75f ? "Snow" : "Heavy snow"));
	else if(g_cells.today.rain > 0.0f || g_cells.today.snow > 0.0f)
		text = "Dry for now";
	else if(w->cloud < 0.2f)
		text = "Clear";
	else if(w->cloud < 0.55f)
		text = "Partly cloudy";
	else
		text = w->cloud < 0.85f ? "Cloudy" : "Overcast";
	snprintf(out, size, "%s", text);
}

void weather_now_start_cell(int sharp, int thunder) {
	// Not primed again: it eases from whatever the weather is now, over a clear day
	g_storm_on = 0;
	g_cell_on = 1;
	g_cell_made = 0;
	g_cell_sharp = sharp;
	g_cell_thunder = thunder;
	g_forced_at_set = 0;
	g_forced_roll = 0;
	memset(&g_forced_day, 0, sizeof(g_forced_day));
	g_forced_day.condition = WEATHER_CLEAR;
	g_forced_day.wind = 1;
	g_forced = WEATHER_CLEAR;
}

int weather_now_cell_running(void) {
	return g_cell_on;
}

void weather_now_start_storm(double seconds, int thunder) {
	g_cell_on = 0;
	// Not primed again: it eases from whatever the weather is now
	g_storm_on = seconds > 0.0;
	g_storm_began = 0;
	g_storm_length = seconds;
	g_storm_thunder = thunder;
	g_forced_at_set = 0;
	g_forced_roll = 0;
	if(!g_storm_on) {
		weather_now_force_day(NULL, 0);
		return;
	}
	memset(&g_forced_day, 0, sizeof(g_forced_day));
	g_forced_day.condition = WEATHER_RAIN;
	g_forced_day.intensity = 2;
	g_forced_day.wind = 1;
	g_forced = WEATHER_RAIN;
}

int weather_now_storm_progress(double* elapsed, double* length) {
	if(!g_storm_on || !g_storm_began)
		return 0;
	*elapsed = weather_state_shared_now_s() - g_storm_at;
	*length = g_storm_length;

	return 1;
}

void weather_now_force_day(const WeatherDayData* day, int roll) {
	// The next update jumps straight to it, so a change can be seen at once
	g_storm_on = 0;
	g_cell_on = 0;
	g_primed = 0;
	g_forced_at_set = 0;
	if(!day || day->condition >= WEATHER_CONDITION_COUNT) {
		g_forced = -1;
		return;
	}
	g_forced = day->condition;
	g_forced_day = *day;
	g_forced_roll = roll;
	if(roll)
		g_primed = 1;
}

void weather_now_force(int condition) {
	WeatherDayData day;

	if(condition < 0 || condition >= WEATHER_CONDITION_COUNT) {
		weather_now_force_day(NULL, 0);
		return;
	}
	memset(&day, 0, sizeof(day));
	day.condition = (uint8_t)condition;
	day.wind = (uint8_t)(condition == WEATHER_THUNDER ? 2 : 1);
	if(condition == WEATHER_RAIN || condition == WEATHER_SNOW)
		day.intensity = 2;
	if(condition == WEATHER_THUNDER)
		day.intensity = 3;
	weather_now_force_day(&day, 0);
}

int weather_now_forced(void) {
	return g_forced;
}

int weather_now_forced_day(WeatherDayData* out) {
	if(g_forced < 0)
		return 0;
	*out = g_forced_day;

	return 1;
}
