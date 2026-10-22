#ifndef WORLD_SEATS_H
#define WORLD_SEATS_H

// The city's seats, baked from the blend's SIT_ markers. Any count loads.

// How close a body must stand to a seat to sit on it: about 3 m, world units.
#define WORLD_SEAT_REACH_UNITS 66.0f

// Longest seat name the file carries, with its terminating NUL.
#define WORLD_SEAT_NAME_MAX 32

typedef struct {
	float x, y; // world units, comparable with a player's position
	float z; // height of the seat, world units
	float heading; // radians; a seated body faces (sin, cos)
	char name[WORLD_SEAT_NAME_MAX];
} world_seat;

// Returns 1 on success (no seats is a success), 0 and nothing loaded otherwise.
int world_seats_load(const char* path);
void world_seats_free(void);

// How many seats are loaded; 0 before a load or after a refused file.
int world_seats_count(void);

// The seat at an index, or NULL when out of range.
const world_seat* world_seats_get(int index);

// Index of the nearest seat within max_dist world units, or -1.
int world_seats_nearest(float world_x, float world_y, float max_dist);

// Seats named with this prefix belong to the shopkeepers, not to players.
#define WORLD_SEAT_KEEPER_PREFIX "SIT_Keeper"

// How near an NPC must stand to a keeper seat to be sitting on it, world units.
#define WORLD_SEAT_KEEPER_MATCH_UNITS 24.0f

// How far a seat is above the floor under it, world units (0.45 m). A sitting
// NPC's height is its seat's, not the spans', which read the counter's top.
#define WORLD_SEAT_HEIGHT_UNITS 9.84f

// 1 when the seat is a shopkeeper's, 0 for any other seat or NULL.
int world_seat_is_reserved(const world_seat* seat);

// Index of the nearest shopkeeper seat within max_dist world units, or -1.
int world_seats_keeper_near(float world_x, float world_y, float max_dist);

#endif /* WORLD_SEATS_H */
