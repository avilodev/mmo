#ifndef WORLD_CITY_LAMPS_H
#define WORLD_CITY_LAMPS_H

// The lamps that light the city at night, read from a data file. No GL.
//
// The file lists each lamp where it stands in the city scene, in metres:
//   { "lamps": [ { "x": -9.88, "y": 3.1, "z": -8.42, "radius": 11, "intensity": 0.9 } ] }
// radius (metres) and intensity are optional.

#define CITY_LAMPS_MAX 32

typedef struct {
	float pos[3];    // world units, GL world space (+X east, +Y up, +Z south)
	float radius;    // world units; the light is gone by here
	float color[3];  // the lamp's warm colour times its intensity
} city_lamp;

// Read the lamps from a file, turning scene metres into world units: the scene
// sits at (origin_x, origin_y) and `units_per_metre` big. Replaces any lamps
// held. Returns the number read, or -1 when the file is missing or broken
// (no lamps are held then).
int city_lamps_load(const char* path, float origin_x, float origin_y, float units_per_metre);

// Forget every lamp
void city_lamps_clear(void);

// How many lamps are held
int city_lamps_count(void);

// The lamps nearest to the ground point (x, y), nearest first, at most `max`.
// Returns how many were written to `out`.
int city_lamps_nearest(float x, float y, int max, city_lamp* out);

#endif /* WORLD_CITY_LAMPS_H */
