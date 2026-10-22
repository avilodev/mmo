// Check the client's emote logic: table, check, phases, seat slide, wording.

#include "player/emote.h"
#include "player/emote_clips.h"
#include "world_seats.h"
#include "world_regions.h"

#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define TILE ((float)WORLD_TILE_PX)
#define SCRATCH "/tmp/emote_test_seats.bin"

static const float OX = CITY_ENNARA_TILE_X * TILE;
static const float OY = CITY_ENNARA_TILE_Y * TILE;

// Clip lengths: sit down 1 s, get up 0.5 s, the rest unused here.
static float g_durations[CHARACTER_ANIM_COUNT];

static void set_durations(void) {
	memset(g_durations, 0, sizeof(g_durations));
	g_durations[CHARACTER_ANIM_SIT_ENTER] = 1.0f;
	g_durations[CHARACTER_ANIM_SIT_EXIT] = 0.5f;
}

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

static void step(emote_state* s, float seconds) {
	for(int i = 0; i < (int)(seconds * 60.0f + 0.5f); i++)
		emote_update(s, 1.0f / 60.0f, g_durations);
}

static float angle_gap(float a, float b) {
	return fabsf(remainderf(a - b, 6.28318530718f));
}

static void test_registry(void) {
	const emote_def* sit = emote_lookup(EMOTE_SIT);
	const emote_def* dance = emote_lookup(EMOTE_DANCE);

	assert(emote_lookup(EMOTE_NONE) == NULL);
	assert(emote_lookup(EMOTE_COUNT) == NULL);
	assert(emote_lookup(255) == NULL);
	assert(sit && dance);
	assert(strcmp(sit->command, "sit") == 0 && strcmp(dance->command, "dance") == 0);
	assert(sit->anchor == EMOTE_ANCHOR_SEAT && dance->anchor == EMOTE_ANCHOR_NONE);
	assert(strcmp(sit->verb, "Sits") == 0 && strcmp(sit->stop_text, "Stands") == 0);
	assert(strcmp(dance->verb, "Dances") == 0);

	// every emote the protocol names has a row, with a command nobody else uses
	for(int id = EMOTE_NONE + 1; id < EMOTE_COUNT; id++) {
		const emote_def* d = emote_lookup((uint8_t)id);

		assert(d && (int)d->id == id && d->command[0] && d->verb[0] && d->stop_text[0]);
		assert(emote_find_command(d->command, strlen(d->command)) == d);
	}
	assert(emote_find_command("sit", 3) == sit);
	assert(emote_find_command("dance", 5) == dance);
	assert(emote_find_command("si", 2) == NULL);
	assert(emote_find_command("stand", 5) == NULL);
}

static void test_check(void) {
	const emote_def* sit = emote_lookup(EMOTE_SIT);
	const emote_def* dance = emote_lookup(EMOTE_DANCE);
	emote_occupant on_first[] = { { EMOTE_SIT, 0 } };
	emote_occupant dancer[] = { { EMOTE_DANCE, 0 } };
	float between = 5.0f * TILE;
	uint8_t arg = 99;

	// dance: anywhere, no seat
	assert(emote_check(dance, OX + 5000.0f, OY, 0, NULL, 0, &arg) == EMOTE_RESULT_STARTED);
	assert(arg == 0);
	assert(emote_check(dance, OX, OY, 1, NULL, 0, &arg) == EMOTE_REFUSED_DEAD);

	// sit: the nearest seat within reach, and its index is the argument
	assert(emote_check(sit, OX + 10.0f, OY, 0, NULL, 0, &arg) == EMOTE_RESULT_STARTED);
	assert(arg == 0);
	assert(emote_check(sit, OX + 10.0f * TILE - 5.0f, OY, 0, NULL, 0, &arg) ==
	       EMOTE_RESULT_STARTED);
	assert(arg == 1);
	assert(emote_check(sit, OX + between, OY + 400.0f, 0, NULL, 0, &arg) ==
	       EMOTE_REFUSED_NO_SEAT);
	assert(emote_check(sit, OX, OY, 1, NULL, 0, &arg) == EMOTE_REFUSED_DEAD);

	// a seat someone visible is on is not offered; in reach of none is no seat
	assert(emote_check(sit, OX + 5.0f, OY, 0, on_first, 1, &arg) ==
	       EMOTE_REFUSED_SEAT_TAKEN);
	assert(emote_check(sit, OX + between, OY, 0, on_first, 1, &arg) ==
	       EMOTE_REFUSED_NO_SEAT);

	// someone dancing is not on a seat
	assert(emote_check(sit, OX + 5.0f, OY, 0, dancer, 1, &arg) == EMOTE_RESULT_STARTED);
}

static void test_keeper_seat(void) {
	const emote_def* sit = emote_lookup(EMOTE_SIT);
	const world_seat* seat;
	emote_pose p;
	CharacterAnim clip = CHARACTER_ANIM_IDLE;
	uint8_t arg = 99;

	load_seats("SIT_Keeper_A");
	seat = world_seats_get(1);

	// a player is never offered a shopkeeper's seat, even standing on it
	assert(emote_check(sit, OX + 10.0f * TILE, OY, 0, NULL, 0, &arg) ==
	       EMOTE_REFUSED_SEAT_TAKEN);
	assert(emote_check(sit, OX + 5.0f, OY, 0, NULL, 0, &arg) == EMOTE_RESULT_STARTED);
	assert(arg == 0);

	// a shopkeeper on that seat sits there, facing as the seat does
	assert(emote_keeper_pose(seat->x, seat->y, &p, &clip) == 1);
	assert(p.x == seat->x && p.y == seat->y);
	assert(p.z == seat->z - EMOTE_SEAT_CLIP_HEIGHT && p.heading == seat->heading);
	assert(clip == CHARACTER_ANIM_SIT_IDLE);

	// a little off the seat still counts; the match radius is the edge
	assert(emote_keeper_pose(seat->x + EMOTE_KEEPER_MATCH_UNITS - 1.0f, seat->y, &p, &clip) == 1);
	assert(emote_keeper_pose(seat->x + EMOTE_KEEPER_MATCH_UNITS + 1.0f, seat->y, &p, &clip) == 0);

	// standing at an ordinary bench is not a shopkeeper's seat
	assert(emote_keeper_pose(OX, OY, &p, &clip) == 0);
	load_two_seats();
	assert(emote_keeper_pose(OX + 10.0f * TILE, OY, &p, &clip) == 0);
}

static void test_sit_timeline(void) {
	emote_state s = { 0 };

	assert(!emote_holds_body(&s));
	emote_begin(&s, EMOTE_SIT, 0);
	assert(s.phase == EMOTE_PHASE_ENTERING && s.id == EMOTE_SIT && s.arg == 0);
	assert(emote_holds_body(&s) && emote_clip(&s) == CHARACTER_ANIM_SIT_ENTER);
	step(&s, 0.5f);
	assert(s.phase == EMOTE_PHASE_ENTERING);
	step(&s, 0.6f);
	assert(s.phase == EMOTE_PHASE_HELD && emote_clip(&s) == CHARACTER_ANIM_SIT_IDLE);
	step(&s, 5.0f);
	assert(s.phase == EMOTE_PHASE_HELD);

	emote_end(&s);
	assert(s.phase == EMOTE_PHASE_EXITING && emote_clip(&s) == CHARACTER_ANIM_SIT_EXIT);
	step(&s, 0.3f);
	assert(s.phase == EMOTE_PHASE_EXITING);
	step(&s, 0.3f);
	assert(s.phase == EMOTE_PHASE_NONE && !emote_holds_body(&s));
	assert(s.id == EMOTE_NONE);
}

static void test_dance_has_no_enter_or_exit(void) {
	emote_state s = { 0 };

	emote_begin(&s, EMOTE_DANCE, 0);
	assert(s.phase == EMOTE_PHASE_HELD && emote_clip(&s) == CHARACTER_ANIM_DANCE);
	emote_end(&s);
	assert(s.phase == EMOTE_PHASE_NONE && !emote_holds_body(&s));
}

// An emote whose arg names a clip: held at once, anchored nowhere, and a new clip starts over
static void test_custom_clip_emote(void) {
	const emote_def* custom = emote_lookup(EMOTE_CUSTOM);
	emote_state s = { 0 };
	uint8_t arg = 9;

	assert(custom && strcmp(custom->command, "emote") == 0);
	assert(custom->anchor == EMOTE_ANCHOR_NONE && custom->hold == CHARACTER_ANIM_CUSTOM);
	assert(custom->enter == CHARACTER_ANIM_CUSTOM_ENTER && custom->exit == CHARACTER_ANIM_CUSTOM_EXIT);

	// no seat is needed, so asking never fails but for the dead
	assert(emote_check(custom, 5000.0f, 5000.0f, 0, NULL, 0, &arg) == EMOTE_RESULT_STARTED);
	assert(emote_check(custom, 0.0f, 0.0f, 1, NULL, 0, &arg) == EMOTE_REFUSED_DEAD);

	emote_begin(&s, EMOTE_CUSTOM, 7);
	assert(s.phase == EMOTE_PHASE_HELD && s.arg == 7 && emote_clip(&s) == CHARACTER_ANIM_CUSTOM);
	assert(!emote_pins_body(&s) && emote_holds_body(&s));

	// another player's clip changing is seen, and the same clip is left alone
	s.timer = 4.0f;
	emote_observe(&s, EMOTE_CUSTOM, 7, 0);
	assert(s.timer == 4.0f);
	emote_observe(&s, EMOTE_CUSTOM, 8, 0);
	assert(s.arg == 8 && s.timer == 0.0f);

	emote_end(&s);
	assert(s.phase == EMOTE_PHASE_NONE);
}

// A clip that comes in a set plays its intro, holds, then plays its exit
static void test_custom_clip_set(void) {
	int middle = emote_clip_find("Slide_Loop");
	int plain = emote_clip_find("Dance_Loop");
	emote_state s = { 0 };

	g_durations[CHARACTER_ANIM_CUSTOM_ENTER] = 1.0f;
	g_durations[CHARACTER_ANIM_CUSTOM_EXIT] = 0.5f;

	emote_begin(&s, EMOTE_CUSTOM, (uint8_t)middle);
	assert(s.phase == EMOTE_PHASE_ENTERING && emote_clip(&s) == CHARACTER_ANIM_CUSTOM_ENTER);
	assert(emote_custom_clip(&s) == emote_clip_find("Slide_Start"));
	step(&s, 0.5f);
	assert(s.phase == EMOTE_PHASE_ENTERING);
	step(&s, 0.6f);
	assert(s.phase == EMOTE_PHASE_HELD && emote_clip(&s) == CHARACTER_ANIM_CUSTOM);
	assert(emote_custom_clip(&s) == middle);

	emote_end(&s);
	assert(s.phase == EMOTE_PHASE_EXITING && emote_clip(&s) == CHARACTER_ANIM_CUSTOM_EXIT);
	assert(emote_custom_clip(&s) == emote_clip_find("Slide_Exit"));
	step(&s, 0.6f);
	assert(s.phase == EMOTE_PHASE_NONE);

	// a clip with no set is held at once and ends at once
	emote_begin(&s, EMOTE_CUSTOM, (uint8_t)plain);
	assert(s.phase == EMOTE_PHASE_HELD && emote_custom_clip(&s) == plain);
	emote_end(&s);
	assert(s.phase == EMOTE_PHASE_NONE);

	// a body first seen mid-emote has no intro to play
	emote_observe(&s, EMOTE_CUSTOM, (uint8_t)middle, 1);
	assert(s.phase == EMOTE_PHASE_HELD);
	assert(emote_custom_clip(&s) == middle);

	memset(&s, 0, sizeof(s));
	emote_begin(&s, EMOTE_DANCE, 0);
	assert(emote_custom_clip(&s) == -1);
}

static void test_begin_end_edges(void) {
	emote_state s = { 0 };

	// nothing to end, not an emote, and one this client does not know
	emote_end(&s);
	assert(s.phase == EMOTE_PHASE_NONE);
	emote_begin(&s, EMOTE_NONE, 0);
	assert(s.phase == EMOTE_PHASE_NONE);
	emote_begin(&s, 200, 0);
	assert(s.phase == EMOTE_PHASE_NONE);

	// a second end changes nothing
	emote_begin(&s, EMOTE_SIT, 0);
	emote_end(&s);
	emote_end(&s);
	assert(s.phase == EMOTE_PHASE_EXITING);

	// a new emote replaces one that is ending
	emote_begin(&s, EMOTE_DANCE, 0);
	assert(s.id == EMOTE_DANCE && s.phase == EMOTE_PHASE_HELD);
}

static void test_seat_vanishing_frees_the_body(void) {
	emote_state s = { 0 };

	emote_begin(&s, EMOTE_SIT, 0);
	world_seats_free();
	step(&s, 1.0f / 60.0f);
	assert(s.phase == EMOTE_PHASE_NONE);
	load_two_seats();

	// a seat that is not there
	emote_begin(&s, EMOTE_SIT, 5);
	step(&s, 1.0f / 60.0f);
	assert(s.phase == EMOTE_PHASE_NONE);
}

static void test_pinning(void) {
	emote_state s = { 0 };

	assert(!emote_pins_body(&s));
	emote_begin(&s, EMOTE_SIT, 0);
	assert(emote_pins_body(&s));
	step(&s, 2.0f);
	assert(emote_pins_body(&s));

	// still pinned until the get-up is done
	emote_end(&s);
	assert(emote_pins_body(&s));
	step(&s, 1.0f);
	assert(!emote_pins_body(&s));

	// a dance does not pin: moving ends it and walks
	emote_begin(&s, EMOTE_DANCE, 0);
	assert(emote_holds_body(&s) && !emote_pins_body(&s));
}

static void test_draw_pose(void) {
	emote_state s = { 0 };
	emote_state t = { 0 };
	emote_pose p;
	const float rx = OX + 20.0f;
	const float ry = OY + 10.0f;
	const float rz = 3.0f;
	const float rh = 3.0f;
	const world_seat* seat = world_seats_get(0);
	float seated_z = seat->z - EMOTE_SEAT_CLIP_HEIGHT;
	float from = seat->heading - 6.0f + 0.2f;

	// standing and dancing: drawn where the body is
	assert(emote_draw_pose(&s, rx, ry, rz, rh, g_durations, &p) == 0);
	emote_begin(&s, EMOTE_DANCE, 0);
	assert(emote_draw_pose(&s, rx, ry, rz, rh, g_durations, &p) == 0);
	emote_end(&s);

	// a sit starts where the body stands
	emote_begin(&s, EMOTE_SIT, 0);
	assert(emote_draw_pose(&s, rx, ry, rz, rh, g_durations, &p) == 1);
	assert(p.x == rx && p.y == ry && p.z == rz && p.heading == rh);

	step(&s, 0.5f);
	assert(emote_draw_pose(&s, rx, ry, rz, rh, g_durations, &p) == 1);
	assert(fabsf(p.x - (rx + seat->x) * 0.5f) < 0.5f);
	assert(fabsf(p.z - (rz + seated_z) * 0.5f) < 0.5f);

	step(&s, 1.0f);
	assert(emote_draw_pose(&s, rx, ry, rz, rh, g_durations, &p) == 1);
	assert(p.x == seat->x && p.y == seat->y && p.z == seated_z);
	assert(p.heading == seat->heading);

	// getting up starts on the seat and ends where the body stood
	emote_end(&s);
	assert(emote_draw_pose(&s, rx, ry, rz, rh, g_durations, &p) == 1);
	assert(p.x == seat->x);
	step(&s, 29.0f / 60.0f);
	assert(emote_draw_pose(&s, rx, ry, rz, rh, g_durations, &p) == 1);
	assert(fabsf(p.x - rx) < 1.0f && fabsf(p.y - ry) < 1.0f);
	assert(fabsf(p.heading - rh) < 0.1f);

	// it turns the short way round
	emote_begin(&t, EMOTE_SIT, 0);
	step(&t, 0.5f);
	assert(emote_draw_pose(&t, OX, OY, 0.0f, from, g_durations, &p) == 1);
	assert(angle_gap(p.heading, from) < 0.3f);
	assert(angle_gap(p.heading, seat->heading) < 0.3f);
}

// What the server says about another player drives their state machine.
static void test_observe(void) {
	emote_state s = { 0 };
	emote_state n = { 0 };

	// seen already sitting: seated at once, with no sit-down to play
	emote_observe(&s, EMOTE_SIT, 1, 1);
	assert(s.phase == EMOTE_PHASE_HELD && s.id == EMOTE_SIT && s.arg == 1);

	// still sitting: nothing changes
	emote_observe(&s, EMOTE_SIT, 1, 0);
	assert(s.phase == EMOTE_PHASE_HELD);

	// the server stops it: they get up
	emote_observe(&s, EMOTE_NONE, 0, 0);
	assert(s.phase == EMOTE_PHASE_EXITING);
	step(&s, 0.6f);
	assert(s.phase == EMOTE_PHASE_NONE);

	// a player already known starts one: it plays from the beginning
	emote_observe(&s, EMOTE_SIT, 0, 0);
	assert(s.phase == EMOTE_PHASE_ENTERING);

	// switching emote restarts, and a dance ends at once
	emote_observe(&s, EMOTE_DANCE, 0, 0);
	assert(s.id == EMOTE_DANCE && s.phase == EMOTE_PHASE_HELD);
	emote_observe(&s, EMOTE_NONE, 0, 0);
	assert(s.phase == EMOTE_PHASE_NONE);

	// a first sighting of nobody emoting, and an id this client does not know
	emote_observe(&n, EMOTE_NONE, 0, 1);
	assert(n.phase == EMOTE_PHASE_NONE);
	emote_observe(&n, 200, 0, 1);
	assert(n.phase == EMOTE_PHASE_NONE);
}

// What the player is told, and what happens to the body, on a server event.
static void test_reactions(void) {
	emote_state none = { 0 };
	emote_state sitting = { EMOTE_SIT, 0, EMOTE_PHASE_HELD, 0.0f };
	emote_state dancing = { EMOTE_DANCE, 0, EMOTE_PHASE_HELD, 0.0f };
	emote_state getting_up = { EMOTE_SIT, 0, EMOTE_PHASE_EXITING, 0.1f };
	emote_event ev;
	emote_reaction r;
	char big[200];
	char line[EMOTE_TEXT_MAX];

	// it worked: "<Name> <Verb>", and nothing changes for the body
	ev = (emote_event){ EMOTE_SIT, 0, EMOTE_RESULT_STARTED, EMOTE_SIT };
	r = emote_react(&ev, "Panda", &sitting);
	assert(r.say == EMOTE_SAY_EMOTE && strcmp(r.text, "Panda Sits") == 0);
	assert(!r.end_local);
	ev = (emote_event){ EMOTE_DANCE, 0, EMOTE_RESULT_STARTED, EMOTE_DANCE };
	r = emote_react(&ev, "Panda", &dancing);
	assert(r.say == EMOTE_SAY_EMOTE && strcmp(r.text, "Panda Dances") == 0);

	// an acknowledged stop says nothing: the player said it themselves
	ev = (emote_event){ EMOTE_NONE, 0, EMOTE_RESULT_STOPPED, EMOTE_NONE };
	r = emote_react(&ev, "Panda", &none);
	assert(r.say == EMOTE_SAY_NONE && !r.end_local);

	// refused: a red "Unable to <emote>", and the predicted emote is taken back
	ev = (emote_event){ EMOTE_NONE, 0, EMOTE_REFUSED_NO_SEAT, EMOTE_SIT };
	r = emote_react(&ev, "Panda", &sitting);
	assert(r.say == EMOTE_SAY_ERROR && strcmp(r.text, "Unable to sit") == 0);
	assert(r.end_local);
	ev = (emote_event){ EMOTE_NONE, 0, EMOTE_REFUSED_COMBAT, EMOTE_DANCE };
	r = emote_react(&ev, "Panda", &none);
	assert(r.say == EMOTE_SAY_ERROR && strcmp(r.text, "Unable to dance") == 0);
	assert(!r.end_local);

	// refused while in a different emote: that one carries on
	ev = (emote_event){ EMOTE_DANCE, 0, EMOTE_REFUSED_SEAT_TAKEN, EMOTE_SIT };
	r = emote_react(&ev, "Panda", &dancing);
	assert(r.say == EMOTE_SAY_ERROR && !r.end_local);

	// every refusal reason is an error
	for(int why = EMOTE_REFUSED_UNKNOWN; why <= EMOTE_REFUSED_COMBAT; why++) {
		ev = (emote_event){ EMOTE_NONE, 0, (uint8_t)why, EMOTE_SIT };
		assert(emote_react(&ev, "Panda", &none).say == EMOTE_SAY_ERROR);
	}

	// a request for something this client has never heard of
	ev = (emote_event){ EMOTE_NONE, 0, EMOTE_REFUSED_UNKNOWN, 200 };
	r = emote_react(&ev, "Panda", &none);
	assert(r.say == EMOTE_SAY_ERROR && strcmp(r.text, "Unable to emote") == 0);

	// the server ended it (moved, died, fought): they stand, and are told so
	for(int why = EMOTE_ENDED_MOVED; why <= EMOTE_ENDED_COMBAT; why++) {
		ev = (emote_event){ EMOTE_NONE, 0, (uint8_t)why, EMOTE_SIT };
		r = emote_react(&ev, "Panda", &sitting);
		assert(r.say == EMOTE_SAY_EMOTE && strcmp(r.text, "Panda Stands") == 0);
		assert(r.end_local);
	}
	ev = (emote_event){ EMOTE_NONE, 0, EMOTE_ENDED_MOVED, EMOTE_DANCE };
	r = emote_react(&ev, "Panda", &dancing);
	assert(strcmp(r.text, "Panda Stops Dancing") == 0 && r.end_local);

	// already stood up by the player: not announced twice
	ev = (emote_event){ EMOTE_NONE, 0, EMOTE_ENDED_MOVED, EMOTE_SIT };
	r = emote_react(&ev, "Panda", &getting_up);
	assert(r.say == EMOTE_SAY_NONE && !r.end_local);
	r = emote_react(&ev, "Panda", &none);
	assert(r.say == EMOTE_SAY_NONE && !r.end_local);

	// a long name never overflows the line
	memset(big, 'N', sizeof(big) - 1);
	big[sizeof(big) - 1] = '\0';
	ev = (emote_event){ EMOTE_SIT, 0, EMOTE_RESULT_STARTED, EMOTE_SIT };
	r = emote_react(&ev, big, &sitting);
	assert(strlen(r.text) < sizeof(r.text));

	// the words said when the player stands themselves
	emote_say_stop(line, sizeof(line), "Panda", emote_lookup(EMOTE_SIT));
	assert(strcmp(line, "Panda Stands") == 0);
	emote_say_stop(line, sizeof(line), "Panda", emote_lookup(EMOTE_DANCE));
	assert(strcmp(line, "Panda Stops Dancing") == 0);
}

int main(void) {
	set_durations();
	load_two_seats();
	test_registry();
	test_check();
	test_keeper_seat();
	test_sit_timeline();
	test_dance_has_no_enter_or_exit();
	test_custom_clip_emote();
	test_custom_clip_set();
	test_begin_end_edges();
	test_seat_vanishing_frees_the_body();
	test_pinning();
	test_draw_pose();
	test_observe();
	test_reactions();
	world_seats_free();
	printf("emote_test: OK\n");

	return 0;
}
