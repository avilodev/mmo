// Check the server's emote rules: refusals, seats, and ending emotes itself.

#include "emote_validator.h"
#include "world_regions.h"
#include "world_seats.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define TILE ((float)WORLD_TILE_PX)
#define SCRATCH "/tmp/emote_validator_seats.bin"

static const float OX = CITY_ENNARA_TILE_X * TILE;
static const float OY = CITY_ENNARA_TILE_Y * TILE;

static int g_taken_seat = -1;
static int g_asked;

// Two seats, at tiles (0, 0) and (10, 0), written as the exporter would; the
// second one carries `second_name`.
static void load_seats(const char* second_name) {
	FILE* f = fopen(SCRATCH, "wb");
	int32_t head[2] = { 1, 2 };
	float rec[2][4] = { { 0.0f, 0.0f, 10.0f, 1.0f }, { 10.0f, 0.0f, 10.0f, 2.0f } };

	assert(f);
	fwrite("CSIT", 1, 4, f);
	fwrite(head, 4, 2, f);
	for(int i = 0; i < 2; i++) {
		char name[32] = { 0 };

		if(i == 1)
			strncpy(name, second_name, sizeof(name) - 1);
		fwrite(rec[i], 4, 4, f);
		fwrite(name, 1, sizeof(name), f);
	}
	fclose(f);
	assert(world_seats_load(SCRATCH) == 1);
}

static void load_two_seats(void) {
	load_seats("");
}

static emote_actor at(float dx, float dy) {
	emote_actor a = { OX + dx, OY + dy, 0, 0 };

	return a;
}

static int taken_seat(void* ctx, int seat) {
	(void)ctx;
	g_asked++;

	return seat == g_taken_seat;
}

static void test_basics(void) {
	emote_actor a = at(0.0f, 0.0f);

	assert(emote_validate(EMOTE_NONE, 0, &a, NULL, NULL) == EMOTE_RESULT_STOPPED);
	assert(emote_validate(EMOTE_COUNT, 0, &a, NULL, NULL) == EMOTE_REFUSED_UNKNOWN);
	assert(emote_validate(255, 0, &a, NULL, NULL) == EMOTE_REFUSED_UNKNOWN);

	// the dead and the fighting do not emote, and being dead comes first
	a.dead = 1;
	assert(emote_validate(EMOTE_DANCE, 0, &a, NULL, NULL) == EMOTE_REFUSED_DEAD);
	a.in_combat = 1;
	assert(emote_validate(EMOTE_DANCE, 0, &a, NULL, NULL) == EMOTE_REFUSED_DEAD);
	a.dead = 0;
	assert(emote_validate(EMOTE_DANCE, 0, &a, NULL, NULL) == EMOTE_REFUSED_COMBAT);
	assert(emote_validate(EMOTE_SIT, 0, &a, NULL, NULL) == EMOTE_REFUSED_COMBAT);

	// stopping is always allowed, even dead or fighting
	a.dead = 1;
	assert(emote_validate(EMOTE_NONE, 0, &a, NULL, NULL) == EMOTE_RESULT_STOPPED);
}

static void test_dance_needs_no_seat(void) {
	emote_actor far_away = at(5000.0f, 5000.0f);

	assert(emote_validate(EMOTE_DANCE, 0, &far_away, NULL, NULL) == EMOTE_RESULT_STARTED);
	assert(emote_validate(EMOTE_DANCE, 77, &far_away, NULL, NULL) == EMOTE_RESULT_STARTED);
}

static void test_custom_names_a_clip(void) {
	emote_actor far_away = at(5000.0f, 5000.0f);
	emote_actor a = at(0.0f, 0.0f);

	assert(emote_validate(EMOTE_CUSTOM, 0, &far_away, NULL, NULL) == EMOTE_RESULT_STARTED);
	assert(emote_validate(EMOTE_CUSTOM, EMOTE_CLIP_COUNT - 1, &far_away, NULL, NULL) ==
	       EMOTE_RESULT_STARTED);
	assert(emote_validate(EMOTE_CUSTOM, EMOTE_CLIP_COUNT, &far_away, NULL, NULL) ==
	       EMOTE_REFUSED_UNKNOWN);
	assert(emote_validate(EMOTE_CUSTOM, 255, &far_away, NULL, NULL) == EMOTE_REFUSED_UNKNOWN);

	// the dead and the fighting do not emote this way either
	a.dead = 1;
	assert(emote_validate(EMOTE_CUSTOM, 3, &a, NULL, NULL) == EMOTE_REFUSED_DEAD);
	a.dead = 0;
	a.in_combat = 1;
	assert(emote_validate(EMOTE_CUSTOM, 3, &a, NULL, NULL) == EMOTE_REFUSED_COMBAT);
}

static void test_sit_reach(void) {
	float slack_edge = WORLD_SEAT_REACH_UNITS + EMOTE_SEAT_REACH_SLACK;
	emote_actor a = at(WORLD_SEAT_REACH_UNITS - 1.0f, 0.0f);

	assert(emote_validate(EMOTE_SIT, 0, &a, NULL, NULL) == EMOTE_RESULT_STARTED);

	// inside the slack, and just past it
	a = at(slack_edge - 1.0f, 0.0f);
	assert(emote_validate(EMOTE_SIT, 0, &a, NULL, NULL) == EMOTE_RESULT_STARTED);
	a = at(slack_edge + 1.0f, 0.0f);
	assert(emote_validate(EMOTE_SIT, 0, &a, NULL, NULL) == EMOTE_REFUSED_NO_SEAT);

	// reach is measured to the seat asked for, not the nearest one
	a = at(10.0f * TILE, 0.0f);
	assert(emote_validate(EMOTE_SIT, 0, &a, NULL, NULL) == EMOTE_REFUSED_NO_SEAT);
	assert(emote_validate(EMOTE_SIT, 1, &a, NULL, NULL) == EMOTE_RESULT_STARTED);

	// a seat that is not there
	assert(emote_validate(EMOTE_SIT, 2, &a, NULL, NULL) == EMOTE_REFUSED_NO_SEAT);
	assert(emote_validate(EMOTE_SIT, 255, &a, NULL, NULL) == EMOTE_REFUSED_NO_SEAT);
}

static void test_sit_taken(void) {
	emote_actor a = at(0.0f, 0.0f);
	emote_actor far_away = at(5000.0f, 0.0f);

	g_taken_seat = 0;
	g_asked = 0;
	assert(emote_validate(EMOTE_SIT, 0, &a, taken_seat, NULL) == EMOTE_REFUSED_SEAT_TAKEN);
	assert(g_asked == 1);

	// the other seat is the taken one
	g_taken_seat = 1;
	assert(emote_validate(EMOTE_SIT, 0, &a, taken_seat, NULL) == EMOTE_RESULT_STARTED);

	// too far is refused as too far, without asking who is there
	g_asked = 0;
	g_taken_seat = 0;
	assert(emote_validate(EMOTE_SIT, 0, &far_away, taken_seat, NULL) == EMOTE_REFUSED_NO_SEAT);
	assert(g_asked == 0);
}

static void test_keeper_seat_is_never_free(void) {
	emote_actor at_keeper = at(10.0f * TILE, 0.0f);
	emote_actor at_bench = at(0.0f, 0.0f);

	// the second seat is a shopkeeper's: refused as taken, nobody asked
	load_seats("SIT_Keeper_A");
	g_taken_seat = -1;
	g_asked = 0;
	assert(emote_validate(EMOTE_SIT, 1, &at_keeper, taken_seat, NULL) ==
	       EMOTE_REFUSED_SEAT_TAKEN);
	assert(emote_validate(EMOTE_SIT, 1, &at_keeper, NULL, NULL) == EMOTE_REFUSED_SEAT_TAKEN);
	assert(g_asked == 0);

	// out of reach is still out of reach, and the bench is still free
	assert(emote_validate(EMOTE_SIT, 1, &at_bench, NULL, NULL) == EMOTE_REFUSED_NO_SEAT);
	assert(emote_validate(EMOTE_SIT, 0, &at_bench, NULL, NULL) == EMOTE_RESULT_STARTED);
	load_two_seats();
}

static void test_no_seats_loaded(void) {
	emote_actor a = at(0.0f, 0.0f);

	world_seats_free();
	assert(emote_validate(EMOTE_SIT, 0, &a, NULL, NULL) == EMOTE_REFUSED_NO_SEAT);
	assert(emote_validate(EMOTE_DANCE, 0, &a, NULL, NULL) == EMOTE_RESULT_STARTED);
	load_two_seats();
}

static void test_upkeep(void) {
	// nothing wrong, and a fight over before it began: it runs on
	assert(emote_upkeep(0, 100.0, 0.0) == EMOTE_RESULT_STARTED);
	assert(emote_upkeep(0, 100.0, 90.0) == EMOTE_RESULT_STARTED);

	// a fight after it began ends it
	assert(emote_upkeep(0, 100.0, 100.5) == EMOTE_ENDED_COMBAT);

	// dying ends it, and outranks the fight
	assert(emote_upkeep(1, 100.0, 0.0) == EMOTE_ENDED_DEAD);
	assert(emote_upkeep(1, 100.0, 100.5) == EMOTE_ENDED_DEAD);
}

int main(void) {
	load_two_seats();
	test_basics();
	test_dance_needs_no_seat();
	test_custom_names_a_clip();
	test_sit_reach();
	test_sit_taken();
	test_keeper_seat_is_never_free();
	test_no_seats_loaded();
	test_upkeep();
	world_seats_free();
	printf("emote_validator_test: OK\n");

	return 0;
}
