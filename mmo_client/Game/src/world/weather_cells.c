// The patches of weather, and the fronts that bring a new day's.

#include "world/weather_cells.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

// The patches travel this fast along this way, world units a second. A fixed
// course, so the weather never lurches when the wind of a day changes.
#define DRIFT_X 0.88f
#define DRIFT_Y 0.47f
#define DRIFT_SPEED 48.0f

// Sizes of the patches: the big shapes and two finer ones, world units
#define SIZE_BIG 4600.0
#define SIZE_MID 2100.0
#define SIZE_SMALL 900.0

// A front starts this far upwind of the region's middle, past the edge of what
// can be seen, and is this wide from old weather to new
#define FRONT_LEAD 7200.0
#define FRONT_WIDTH 1800.0

// Cloud runs ahead of the rain: the thick sky reaches a spot this far (world
// units) before the rain does, along the way the weather travels
#define CLOUD_LEAD 3600.0

#define NOISE_SEED 0x57A11u

static float clamp01(float v) {
	if(v < 0.0f)
		return 0.0f;

	return v > 1.0f ? 1.0f : v;
}

static float smooth_step(float lo, float hi, float x) {
	float t = clamp01((x - lo) / (hi - lo));

	return t * t * (3.0f - 2.0f * t);
}

static float lerp(float a, float b, float t) {
	return a + (b - a) * t;
}

static uint32_t hash_cell(int64_t x, int64_t y, uint32_t seed) {
	uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u + seed * 2246822519u;

	h = (h ^ (h >> 13)) * 1274126177u;

	return h ^ (h >> 16);
}

// Smooth noise 0..1 at one size, from a position that may be far out (double)
static float value_noise(double x, double y, double size, uint32_t seed) {
	double fx = x / size;
	double fy = y / size;
	double ix = floor(fx);
	double iy = floor(fy);
	float tx = (float)(fx - ix);
	float ty = (float)(fy - iy);
	int64_t cx = (int64_t)ix;
	int64_t cy = (int64_t)iy;
	float v00 = (float)(hash_cell(cx, cy, seed) & 0xFFFFu) / 65535.0f;
	float v10 = (float)(hash_cell(cx + 1, cy, seed) & 0xFFFFu) / 65535.0f;
	float v01 = (float)(hash_cell(cx, cy + 1, seed) & 0xFFFFu) / 65535.0f;
	float v11 = (float)(hash_cell(cx + 1, cy + 1, seed) & 0xFFFFu) / 65535.0f;

	tx = tx * tx * (3.0f - 2.0f * tx);
	ty = ty * ty * (3.0f - 2.0f * ty);

	return lerp(lerp(v00, v10, tx), lerp(v01, v11, tx), ty);
}

void weather_cells_drift(float* dir_x, float* dir_y, float* speed) {
	float len = sqrtf(DRIFT_X * DRIFT_X + DRIFT_Y * DRIFT_Y);

	if(dir_x)
		*dir_x = DRIFT_X / len;
	if(dir_y)
		*dir_y = DRIFT_Y / len;
	if(speed)
		*speed = DRIFT_SPEED;
}

float weather_cells_front_at(double into_day_s) {
	return (float)(-FRONT_LEAD + into_day_s * (double)DRIFT_SPEED);
}

float weather_wind_speed(float level) {
	// Calm, breezy, windy, gale, and a storm's blast, in world units a second
	static const float SPEEDS[5] = { 8.0f, 40.0f, 100.0f, 190.0f, 300.0f };
	int low;
	float t;

	if(level <= 0.0f)
		return SPEEDS[0];
	if(level >= 4.0f)
		return SPEEDS[4];
	low = (int)level;
	t = level - (float)low;

	return lerp(SPEEDS[low], SPEEDS[low + 1], t);
}

// How hard a fall is at the core of a patch and how much ground it reaches, by
// the level of the day: light, moderate, heavy
static const float RAIN_CORE[3] = { 0.28f, 0.55f, 0.92f };
static const float RAIN_COVER[3] = { 0.45f, 0.70f, 0.92f };
static const float SNOW_CORE[3] = { 0.25f, 0.50f, 0.90f };
static const float SNOW_COVER[3] = { 0.50f, 0.75f, 0.95f };
static const float STORM_CORE[3] = { 0.60f, 0.75f, 1.00f };
static const float STORM_COVER[3] = { 0.45f, 0.60f, 0.78f };
// How overcast the sky is where each falls
static const float RAIN_CLOUD[3] = { 0.80f, 0.92f, 1.00f };
static const float SNOW_CLOUD[3] = { 0.78f, 0.88f, 0.97f };

void weather_envelope_of(const WeatherDayData* day, weather_envelope* out) {
	int level = day->intensity;

	memset(out, 0, sizeof(*out));
	out->wind = (float)(day->wind > 3 ? 3 : day->wind);
	if(level < 1)
		level = 2;
	if(level > 3)
		level = 3;
	level -= 1;

	switch(day->condition) {
		case WEATHER_CLEAR:
			out->cloud = 0.04f;
			break;
		case WEATHER_PARTLY_CLOUDY:
			out->cloud = 0.38f;
			break;
		case WEATHER_CLOUDY:
			out->cloud = 0.78f;
			break;
		case WEATHER_RAIN:
			out->cloud = RAIN_CLOUD[level];
			out->rain = RAIN_CORE[level];
			out->coverage = RAIN_COVER[level];
			break;
		case WEATHER_THUNDER:
			out->cloud = 1.0f;
			out->rain = 0.60f + 0.40f * (float)level / 2.0f;
			out->storm = STORM_CORE[level];
			out->coverage = STORM_COVER[level];
			break;
		case WEATHER_SNOW:
			out->cloud = SNOW_CLOUD[level];
			out->snow = SNOW_CORE[level];
			out->coverage = SNOW_COVER[level];
			break;
		default:
			out->cloud = 0.04f;
			break;
	}
}

// The storm's moments: when (a share of its length), then how overcast, how hard
// it rains, how windy, and how stormy. The levels are the ones of a day's
// forecast: light 0.28, moderate 0.55 and heavy 0.92 of rain.
typedef struct {
	double at;
	float cloud;
	float rain;
	float wind;
	float storm;
} storm_moment;

static const storm_moment STORM[] = {
	{ 0.00, 0.04f, 0.002f, 1.0f, 0.0f }, // clear
	{ 0.14, 0.72f, 0.030f, 1.1f, 0.0f }, // clouded over, the first drops
	{ 0.22, 0.80f, 0.280f, 1.2f, 0.0f }, // light
	{ 0.30, 0.80f, 0.280f, 1.2f, 0.0f },
	{ 0.41, 0.92f, 0.550f, 1.6f, 0.0f }, // moderate
	{ 0.46, 0.92f, 0.550f, 1.6f, 0.0f },
	{ 0.57, 1.00f, 0.920f, 2.2f, 1.0f }, // heavy
	{ 0.65, 1.00f, 0.920f, 2.2f, 1.0f },
	{ 0.74, 0.92f, 0.550f, 1.6f, 0.0f }, // moderate again
	{ 0.78, 0.92f, 0.550f, 1.6f, 0.0f },
	{ 0.84, 0.80f, 0.280f, 1.2f, 0.0f }, // light again
	{ 0.91, 0.45f, 0.030f, 1.0f, 0.0f }, // breaking up
	{ 1.00, 0.04f, 0.002f, 1.0f, 0.0f }  // clear
};

#define STORM_MOMENTS ((int)(sizeof(STORM) / sizeof(STORM[0])))

double weather_storm_front_s(double elapsed, double length) {
	double rain_begins = STORM[1].at * length;

	if(rain_begins <= 0.0)
		return 0.0;

	return elapsed * ((double)FRONT_LEAD / (double)DRIFT_SPEED) / rain_begins;
}

// How stormy the heaviest rain is, when the storm has thunder
#define STORM_THUNDER_CORE 0.75f

int weather_storm_envelope(double elapsed, double length, int thunder, weather_envelope* out) {
	double share = length > 0.0 ? elapsed / length : 1.0;
	int i = 0;
	float t;

	memset(out, 0, sizeof(*out));
	if(share < 0.0)
		share = 0.0;
	if(share >= 1.0) {
		out->cloud = STORM[STORM_MOMENTS - 1].cloud;
		out->wind = STORM[STORM_MOMENTS - 1].wind;

		return 0;
	}
	while(i + 2 < STORM_MOMENTS && share >= STORM[i + 1].at)
		i++;
	t = smooth_step(0.0f, 1.0f, (float)((share - STORM[i].at) / (STORM[i + 1].at - STORM[i].at)));
	out->cloud = lerp(STORM[i].cloud, STORM[i + 1].cloud, t);
	// A little rain always: the sky stays one kind of sky all through it
	out->rain = lerp(STORM[i].rain, STORM[i + 1].rain, t);
	out->wind = lerp(STORM[i].wind, STORM[i + 1].wind, t);
	if(thunder)
		out->storm = lerp(STORM[i].storm, STORM[i + 1].storm, t) * STORM_THUNDER_CORE;
	out->coverage = 1.0f;

	return 1;
}

// The patch pattern at a spot and time: 0..1, drifting along the wind
static float pattern_at(double x, double y, double shared_s) {
	double ux = x - (double)(DRIFT_X / sqrtf(DRIFT_X * DRIFT_X + DRIFT_Y * DRIFT_Y)) * (double)DRIFT_SPEED * shared_s;
	double uy = y - (double)(DRIFT_Y / sqrtf(DRIFT_X * DRIFT_X + DRIFT_Y * DRIFT_Y)) * (double)DRIFT_SPEED * shared_s;

	return clamp01(value_noise(ux, uy, SIZE_BIG, NOISE_SEED) * 0.55f +
	               value_noise(ux, uy, SIZE_MID, NOISE_SEED + 1u) * 0.30f +
	               value_noise(ux, uy, SIZE_SMALL, NOISE_SEED + 2u) * 0.15f);
}

// What one day's envelope is at a spot where the pattern is n, and where it is
// n_lead at the place the cloud ahead of the rain comes from
static void weather_under(const weather_envelope* env, float n, float n_lead,
                          weather_cell_sample* out) {
	float falls = env->rain > env->snow ? env->rain : env->snow;
	// The level of the pattern a fall needs: the more ground it covers, the lower
	float threshold = 0.5f + (0.5f - env->coverage) * 0.6f;
	float present = falls > 0.0f ? smooth_step(threshold - 0.05f, threshold + 0.10f, n) : 0.0f;
	// Where a patch of rain is a little upwind, the sky has shut already
	float shut = falls > 0.0f ? smooth_step(threshold - 0.05f, threshold + 0.10f, n_lead) : 0.0f;
	float core = 0.55f + 0.45f * smooth_step(threshold, threshold + 0.3f, n);
	float patchy = clamp01(env->cloud * 0.55f + (n - 0.5f) * 0.5f);

	memset(out, 0, sizeof(*out));
	out->rain = clamp01(env->rain * core * present);
	out->snow = clamp01(env->snow * core * present);
	out->storm = clamp01(env->storm * smooth_step(threshold + 0.10f, threshold + 0.30f, n) * present);
	if(falls > 0.0f) {
		// Between the patches the sky thins; under them it is shut
		out->cloud = lerp(patchy, env->cloud, present > shut ? present : shut);
	} else {
		// A dry day's cloud is broken up too, thicker here and thinner there
		out->cloud = clamp01(env->cloud + (n - 0.5f) * (env->cloud > 0.1f && env->cloud < 0.9f ? 0.9f : 0.3f));
	}
	out->wind = env->wind;
}

// ---- Storm cells ----

// Cells are rolled once in each slot of this many seconds of the shared clock
#define CELL_SLOT_S 300.0

// Past this far (world units) from a cell's middle it has nothing to say
#define CELL_REACH 18000.0

// The slowest a cell travels, to bound which slots can reach a spot
#define CELL_SPEED_MIN 40.0

// How a cell is made, by kind: how far ahead of its middle rain reaches, and
// how wide it is across, in sigmas
#define BROAD_SIGMA 4000.0f
#define SHARP_SIGMA 1200.0f
#define BROAD_WIDTH 2.2f
#define SHARP_WIDTH 4.5f

// How a called storm is made, and how long before its middle arrives you see it
#define CALLED_PEAK_BROAD 0.92f
#define CALLED_PEAK_SHARP 1.0f

static float gauss(float x) {
	return expf(-x * x);
}

static float cell_rand(int64_t slot, uint32_t seed, uint32_t salt) {
	return (float)(hash_cell(slot, (int64_t)salt, seed) & 0xFFFFFFu) / 16777216.0f;
}

// A day's falls and wind without the falls: what a day is between its cells
static weather_envelope dry_of(const weather_envelope* env) {
	weather_envelope out = *env;

	out.rain = 0.0f;
	out.snow = 0.0f;
	out.storm = 0.0f;
	out.coverage = 0.0f;

	return out;
}

// The weather of the day a cell arrives in; 0 when that day is not known
static int envelope_for(const weather_cells_input* in, double arrive_s, weather_envelope* out) {
	if(in->day_length_s <= 0.0) {
		*out = in->today;

		return 1;
	}
	if(arrive_s < in->day_start_s) {
		*out = in->has_before ? in->before : in->today;

		return 1;
	}
	if(arrive_s >= in->day_start_s + in->day_length_s) {
		if(!in->has_after)
			return 0;
		*out = in->after;

		return 1;
	}
	*out = in->today;

	return 1;
}

// The cell rolled for a slot, if the weather of its day has one
static int cell_of_slot(const weather_cells_input* in, int64_t slot, weather_cell* out) {
	uint32_t seed = hash_cell((int64_t)in->centre_x, (int64_t)in->centre_y, 0xC311u);
	double arrive = ((double)slot + (double)cell_rand(slot, seed, 1)) * CELL_SLOT_S;
	weather_envelope day;
	float core;
	float sharp;
	float burst;
	int snow;

	if(!envelope_for(in, arrive, &day))
		return 0;
	snow = day.snow > day.rain;
	core = snow ? day.snow : day.rain;
	if(core <= 0.0f || day.coverage <= 0.0f)
		return 0;
	// Heavier days have a cell more often, and a hard-hitting day more often has a hard one
	if(cell_rand(slot, seed, 2) > 0.20f + 0.70f * day.coverage + (day.storm > 0.0f ? 0.25f : 0.0f))
		return 0;
	sharp = cell_rand(slot, seed, 3) < (day.storm > 0.0f ? 0.65f : 0.3f) ? 1.0f : 0.0f;
	// Now and then a storm hits much harder than the day, but never for long
	burst = cell_rand(slot, seed, 4) < 0.15f ? 1.0f : 0.0f;
	if(burst > 0.0f)
		sharp = 1.0f;

	memset(out, 0, sizeof(*out));
	out->arrive_s = arrive;
	out->sigma = sharp > 0.0f ? SHARP_SIGMA * (0.8f + 0.5f * cell_rand(slot, seed, 5))
	                          : BROAD_SIGMA * (0.8f + 0.5f * cell_rand(slot, seed, 5));
	out->sharp = sharp;
	out->peak = fminf(1.0f, core * (0.85f + 0.3f * cell_rand(slot, seed, 6)) * (burst > 0.0f ? 1.6f : 1.0f));
	out->speed = DRIFT_SPEED * (0.85f + 0.3f * cell_rand(slot, seed, 7));
	out->snow = snow;
	out->thunder = day.storm > 0.0f || (burst > 0.0f && cell_rand(slot, seed, 8) < 0.4f);
	out->cross = (cell_rand(slot, seed, 9) - 0.5f) * 2.0f * out->sigma *
	             (sharp > 0.0f ? SHARP_WIDTH : BROAD_WIDTH) * 0.9f;

	return 1;
}

// What one cell is at a spot: q along the way it goes ahead of its middle (the
// spot is not yet reached while positive), v across it
typedef struct {
	float rain;
	float cloud; // how far the sky is shut, 0..1
	float gust;
	float storm;
} cell_here;

static cell_here cell_at(const weather_cell* c, float q, float v) {
	float sf = c->sigma;
	float sb = c->sigma * (1.5f + 0.7f * c->sharp);
	float power = 2.0f + 2.0f * c->sharp;
	float width = c->sigma * (BROAD_WIDTH + (SHARP_WIDTH - BROAD_WIDTH) * c->sharp);
	float across = gauss(v / width);
	float along = q >= 0.0f ? expf(-powf(q / sf, power)) : gauss(q / sb);
	float ahead = fmaxf(q - (1.0f + c->sharp) * sf, 0.0f) / (2.2f * sf);
	float behind = fmaxf(-q - 0.8f * sb, 0.0f) / (1.8f * sb);
	cell_here out;

	out.rain = along * across * c->peak;
	out.cloud = gauss(sqrtf(ahead * ahead + behind * behind)) * gauss(v / (1.5f * width));
	out.gust = gauss((q - 0.45f * sf) / (0.8f * sf)) * gauss(v / (1.2f * width)) * c->peak;
	out.storm = c->thunder ? clamp01(1.25f * out.rain - 0.1f) * (0.6f + 0.4f * c->sharp) : 0.0f;

	return out;
}

// What a spot has been given by the cells so far
typedef struct {
	float cloud;
	float wind;
} cell_sum;

// Put one cell's say over a spot into its weather
static void apply_cell(const weather_cell* cell, double shared_s, float along, float across,
                       weather_cell_sample* out, cell_sum* sum) {
	float q = along - cell->speed * (float)(shared_s - cell->arrive_s);
	cell_here here;
	float target;
	float shut;

	if(fabsf(q) > (float)CELL_REACH)
		return;
	here = cell_at(cell, q, across - cell->cross);
	if(cell->snow)
		out->snow = fmaxf(out->snow, here.rain);
	else
		out->rain = fmaxf(out->rain, here.rain);
	out->storm = fmaxf(out->storm, here.storm);
	target = 0.75f + 0.25f * cell->peak;
	shut = out->cloud + (fmaxf(target, out->cloud) - out->cloud) * here.cloud;
	if(shut > sum->cloud)
		sum->cloud = shut;
	sum->wind += (0.6f + 1.4f * cell->sharp) * here.gust + (0.5f + 0.2f * cell->sharp) * here.rain;
}

// Add what the cells of a day, and the called one, make of a spot to its weather
static void add_cells(const weather_cells_input* in, float x, float y, weather_cell_sample* out) {
	cell_sum sum;
	float dir_x;
	float dir_y;
	float along;
	float across;

	sum.cloud = out->cloud;
	sum.wind = 0.0f;
	weather_cells_drift(&dir_x, &dir_y, NULL);
	along = (x - in->centre_x) * dir_x + (y - in->centre_y) * dir_y;
	across = (y - in->centre_y) * dir_x - (x - in->centre_x) * dir_y;
	if(in->use_cells) {
		double span = (fabs((double)along) + CELL_REACH) / CELL_SPEED_MIN;
		int64_t first = (int64_t)floor((in->shared_s - span) / CELL_SLOT_S);
		int64_t last = (int64_t)floor((in->shared_s + span) / CELL_SLOT_S);

		for(int64_t slot = first; slot <= last; slot++) {
			weather_cell cell;

			if(cell_of_slot(in, slot, &cell))
				apply_cell(&cell, in->shared_s, along, across, out, &sum);
		}
	}
	if(in->extra_on)
		apply_cell(&in->extra, in->shared_s, along, across, out, &sum);
	out->cloud = clamp01(sum.cloud);
	out->wind = fminf(4.0f, out->wind + sum.wind);
}

void weather_cell_called(const weather_cells_input* in, int sharp, int thunder, double hits_s,
                         float x, float y, weather_cell* out) {
	float dir_x;
	float dir_y;
	float along;

	weather_cells_drift(&dir_x, &dir_y, NULL);
	along = (x - in->centre_x) * dir_x + (y - in->centre_y) * dir_y;
	memset(out, 0, sizeof(*out));
	out->speed = DRIFT_SPEED;
	out->sharp = sharp ? 1.0f : 0.0f;
	out->sigma = sharp ? SHARP_SIGMA : BROAD_SIGMA;
	out->peak = sharp ? CALLED_PEAK_SHARP : CALLED_PEAK_BROAD;
	out->thunder = thunder;
	out->cross = (y - in->centre_y) * dir_x - (x - in->centre_x) * dir_y;
	out->arrive_s = hits_s - (double)along / (double)out->speed;
}

double weather_cell_after_s(const weather_cell* cell) {
	// Its tail is long gone from a spot after this far behind its middle
	return (double)(cell->sigma * (1.5f + 0.7f * cell->sharp) * 2.2f) / (double)cell->speed;
}

void weather_cells_sample(const weather_cells_input* in, float x, float y,
                          weather_cell_sample* out) {
	float n = pattern_at((double)x, (double)y, in->shared_s);
	float dir_x;
	float dir_y;
	float n_lead;
	weather_envelope today = in->today;
	weather_envelope before = in->before;
	weather_cell_sample now;
	weather_cell_sample old;
	float fresh = 1.0f;

	if(in->use_cells) {
		today = dry_of(&today);
		before = dry_of(&before);
	}
	weather_cells_drift(&dir_x, &dir_y, NULL);
	n_lead = pattern_at((double)x - (double)dir_x * CLOUD_LEAD, (double)y - (double)dir_y * CLOUD_LEAD,
	                    in->shared_s);
	weather_under(&today, n, n_lead, &now);
	if(in->has_before) {
		float along = (x - in->centre_x) * dir_x + (y - in->centre_y) * dir_y;
		float front = weather_cells_front_at(in->into_day_s);
		float fresh_cloud;

		// Behind the front, the new weather; ahead of it, the old. The new
		// cloud is out ahead of the new rain.
		fresh = 1.0f - smooth_step(front - (float)FRONT_WIDTH * 0.5f,
		                           front + (float)FRONT_WIDTH * 0.5f, along);
		fresh_cloud = 1.0f - smooth_step(front + (float)CLOUD_LEAD - (float)FRONT_WIDTH * 0.5f,
		                                 front + (float)CLOUD_LEAD + (float)FRONT_WIDTH * 0.5f, along);
		weather_under(&before, n, n_lead, &old);
		out->cloud = lerp(old.cloud, now.cloud, fresh_cloud);
		out->rain = lerp(old.rain, now.rain, fresh);
		out->snow = lerp(old.snow, now.snow, fresh);
		out->storm = lerp(old.storm, now.storm, fresh);
		out->wind = lerp(old.wind, now.wind, fresh);
		out->fresh = fresh;
	} else {
		*out = now;
		out->fresh = 1.0f;
	}
	if(in->use_cells || in->extra_on)
		add_cells(in, x, y, out);
}

void weather_cells_grid(const weather_cells_input* in, float x0, float y0, float cell, int count,
                        weather_cell_sample* out) {
	for(int j = 0; j < count; j++)
		for(int i = 0; i < count; i++)
			weather_cells_sample(in, x0 + (float)i * cell, y0 + (float)j * cell,
			                     &out[j * count + i]);
}

float weather_cells_storm_near(const weather_cells_input* in, float x, float y, float radius) {
	static const float RINGS[4] = { 0.0f, 0.3f, 0.65f, 1.0f };
	float best = 0.0f;

	for(int r = 0; r < 4; r++) {
		int around = r == 0 ? 1 : 12;

		for(int k = 0; k < around; k++) {
			float angle = (float)k / (float)around * 6.2831853f;
			weather_cell_sample s;

			weather_cells_sample(in, x + cosf(angle) * radius * RINGS[r],
			                     y + sinf(angle) * radius * RINGS[r], &s);
			if(s.storm > best)
				best = s.storm;
		}
	}

	return best;
}
