// The emote table, the client-side check, and a body's emote state.

// To add an emote: one row in EMOTES.

#include "player/emote.h"
#include "player/emote_clips.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define TWO_PI 6.28318530718f

static const emote_def EMOTES[] = {
	{ EMOTE_SIT, "sit", "Sits", "Stands", EMOTE_ANCHOR_SEAT,
	  CHARACTER_ANIM_SIT_ENTER, CHARACTER_ANIM_SIT_IDLE, CHARACTER_ANIM_SIT_EXIT },
	{ EMOTE_DANCE, "dance", "Dances", "Stops Dancing", EMOTE_ANCHOR_NONE,
	  EMOTE_NO_CLIP, CHARACTER_ANIM_DANCE, EMOTE_NO_CLIP },
	// the clip is the emote's arg; /emote plays the Emote 1 slot; a clip in a set
// (emote_clips.h) plays its intro and exit, any other has none
	{ EMOTE_CUSTOM, "emote", "Emotes", "Stops Emoting", EMOTE_ANCHOR_NONE,
	  CHARACTER_ANIM_CUSTOM_ENTER, CHARACTER_ANIM_CUSTOM, CHARACTER_ANIM_CUSTOM_EXIT },
	{ EMOTE_LEAN, "lean", "Leans", "Stops Leaning", EMOTE_ANCHOR_NONE,
	  CHARACTER_ANIM_LEAN_ENTER, CHARACTER_ANIM_LEAN_IDLE, CHARACTER_ANIM_LEAN_EXIT },
	{ EMOTE_LEAN_FOOT, "lean2", "Leans", "Stops Leaning", EMOTE_ANCHOR_NONE,
	  CHARACTER_ANIM_LEAN_FOOT_ENTER, CHARACTER_ANIM_LEAN_FOOT_IDLE, CHARACTER_ANIM_LEAN_FOOT_EXIT },
};

#define EMOTE_ROWS ((int)(sizeof(EMOTES) / sizeof(EMOTES[0])))

const emote_def* emote_lookup(uint8_t id) {
	for(int i = 0; i < EMOTE_ROWS; i++)
		if(EMOTES[i].id == id)
			return &EMOTES[i];

	return NULL;
}

const emote_def* emote_find_command(const char* word, size_t length) {
	for(int i = 0; i < EMOTE_ROWS; i++)
		if(strlen(EMOTES[i].command) == length && strncmp(EMOTES[i].command, word, length) == 0)
			return &EMOTES[i];

	return NULL;
}

static int seat_is_occupied(const emote_occupant* others, int count, uint8_t seat) {
	for(int i = 0; i < count; i++)
		if(others[i].id == EMOTE_SIT && others[i].arg == seat)
			return 1;

	return 0;
}

// Picks the nearest free seat within reach.
static EmoteResult choose_seat(float x, float y, const emote_occupant* others,
		int other_count, uint8_t* out_arg) {
	int best = -1;
	int in_reach = 0;
	float best_d = 0.0f;

	for(int i = 0; i < world_seats_count() && i < 256; i++) {
		const world_seat* seat = world_seats_get(i);
		float d = hypotf(seat->x - x, seat->y - y);

		if(d > WORLD_SEAT_REACH_UNITS)
			continue;

		in_reach = 1;
		if(world_seat_is_reserved(seat))
			continue;
		if(seat_is_occupied(others, other_count, (uint8_t)i))
			continue;

		if(best < 0 || d < best_d) {
			best = i;
			best_d = d;
		}
	}

	if(best >= 0) {
		*out_arg = (uint8_t)best;
		return EMOTE_RESULT_STARTED;
	}

	return in_reach ? EMOTE_REFUSED_SEAT_TAKEN : EMOTE_REFUSED_NO_SEAT;
}

EmoteResult emote_check(const emote_def* def, float x, float y, int is_dead,
		const emote_occupant* others, int other_count,
		uint8_t* out_arg) {
	*out_arg = 0;
	if(!def)
		return EMOTE_REFUSED_UNKNOWN;
	if(is_dead)
		return EMOTE_REFUSED_DEAD;
	if(def->anchor == EMOTE_ANCHOR_SEAT)
		return choose_seat(x, y, others, other_count, out_arg);

	return EMOTE_RESULT_STARTED;
}

// Whether this emote plays an intro, or an exit, for this arg
static int has_enter(const emote_def* def, uint8_t arg) {
	if(def->enter == EMOTE_NO_CLIP)
		return 0;

	return def->id != EMOTE_CUSTOM || emote_clip_enter(arg) >= 0;
}

static int has_exit(const emote_def* def, uint8_t arg) {
	if(def->exit == EMOTE_NO_CLIP)
		return 0;

	return def->id != EMOTE_CUSTOM || emote_clip_exit(arg) >= 0;
}

void emote_begin(emote_state* s, uint8_t id, uint8_t arg) {
	const emote_def* def = emote_lookup(id);

	if(!def)
		return;

	s->id = id;
	s->arg = arg;
	s->timer = 0.0f;
	s->phase = has_enter(def, arg) ? EMOTE_PHASE_ENTERING : EMOTE_PHASE_HELD;
}

static void clear(emote_state* s) {
	s->id = EMOTE_NONE;
	s->arg = 0;
	s->phase = EMOTE_PHASE_NONE;
	s->timer = 0.0f;
}

void emote_end(emote_state* s) {
	const emote_def* def = emote_lookup(s->id);

	if(s->phase != EMOTE_PHASE_ENTERING && s->phase != EMOTE_PHASE_HELD)
		return;

	if(!def || !has_exit(def, s->arg)) {
		clear(s);
		return;
	}

	s->phase = EMOTE_PHASE_EXITING;
	s->timer = 0.0f;
}

void emote_update(emote_state* s, float dt,
		const float durations[CHARACTER_ANIM_COUNT]) {
	const emote_def* def = emote_lookup(s->id);

	if(s->phase == EMOTE_PHASE_NONE)
		return;

	// An emote whose seat is gone (the file reloaded, or failed) frees the body.
	if(!def || (def->anchor == EMOTE_ANCHOR_SEAT && !world_seats_get(s->arg))) {
		clear(s);
		return;
	}

	s->timer += dt;
	if(s->phase == EMOTE_PHASE_ENTERING && s->timer >= durations[def->enter]) {
		s->phase = EMOTE_PHASE_HELD;
		s->timer = 0.0f;
	} else if(s->phase == EMOTE_PHASE_EXITING && s->timer >= durations[def->exit]) {
		clear(s);
	}
}

void emote_observe(emote_state* s, uint8_t id, uint8_t arg, int first_sight) {
	int active = s->phase == EMOTE_PHASE_ENTERING || s->phase == EMOTE_PHASE_HELD;

	if(id == EMOTE_NONE) {
		emote_end(s);
		return;
	}

	if(!emote_lookup(id))
		return;
	if(active && s->id == id && s->arg == arg)
		return;

	emote_begin(s, id, arg);
	if(first_sight) {
		s->phase = EMOTE_PHASE_HELD;
		s->timer = 0.0f;
	}
}

int emote_holds_body(const emote_state* s) {
	return s->phase != EMOTE_PHASE_NONE;
}

int emote_pins_body(const emote_state* s) {
	const emote_def* def = emote_lookup(s->id);

	return s->phase != EMOTE_PHASE_NONE && def && def->anchor == EMOTE_ANCHOR_SEAT;
}

CharacterAnim emote_clip(const emote_state* s) {
	const emote_def* def = emote_lookup(s->id);

	if(!def)
		return CHARACTER_ANIM_IDLE;
	if(s->phase == EMOTE_PHASE_ENTERING)
		return def->enter;
	if(s->phase == EMOTE_PHASE_EXITING)
		return def->exit;
	if(s->phase == EMOTE_PHASE_HELD)
		return def->hold;

	return CHARACTER_ANIM_IDLE;
}

int emote_custom_clip(const emote_state* s) {
	if(s->id != EMOTE_CUSTOM)
		return -1;
	if(s->phase == EMOTE_PHASE_ENTERING)
		return emote_clip_enter(s->arg);
	if(s->phase == EMOTE_PHASE_EXITING)
		return emote_clip_exit(s->arg);

	return s->arg;
}

// How far along the slide from where the body stands (0) to the anchor (1).
static float slide_fraction(const emote_state* s, const emote_def* def,
		const float durations[CHARACTER_ANIM_COUNT]) {
	float enter = durations[def->enter];
	float exit = durations[def->exit];
	float t = 1.0f;

	if(s->phase == EMOTE_PHASE_ENTERING)
		t = enter > 0.0f ? s->timer / enter : 1.0f;
	else if(s->phase == EMOTE_PHASE_EXITING)
		t = exit > 0.0f ? 1.0f - s->timer / exit : 0.0f;

	if(t < 0.0f)
		return 0.0f;

	return t > 1.0f ? 1.0f : t;
}

int emote_draw_pose(const emote_state* s, float real_x, float real_y,
		float real_z, float real_heading,
		const float durations[CHARACTER_ANIM_COUNT], emote_pose* out) {
	const emote_def* def = emote_lookup(s->id);
	const world_seat* seat;
	float t;
	float seated_z;
	float turn;

	if(s->phase == EMOTE_PHASE_NONE || !def || def->anchor != EMOTE_ANCHOR_SEAT)
		return 0;

	seat = world_seats_get(s->arg);
	if(!seat)
		return 0;

	t = slide_fraction(s, def, durations);
	seated_z = seat->z - EMOTE_SEAT_CLIP_HEIGHT;
	turn = remainderf(seat->heading - real_heading, TWO_PI);
	out->x = real_x + (seat->x - real_x) * t;
	out->y = real_y + (seat->y - real_y) * t;
	out->z = real_z + (seated_z - real_z) * t;
	out->heading = real_heading + turn * t;

	return 1;
}

int emote_keeper_pose(float x, float y, emote_pose* out, CharacterAnim* clip) {
	float durations[CHARACTER_ANIM_COUNT] = { 0 };
	emote_state held = { 0 };
	int seat = world_seats_keeper_near(x, y, EMOTE_KEEPER_MATCH_UNITS);

	if(seat < 0 || seat > 255)
		return 0;

	emote_begin(&held, EMOTE_SIT, (uint8_t)seat);
	held.phase = EMOTE_PHASE_HELD;
	if(!emote_draw_pose(&held, x, y, 0.0f, 0.0f, durations, out))
		return 0;

	*clip = emote_clip(&held);

	return 1;
}

void emote_say_stop(char* out, size_t size, const char* name,
		const emote_def* def) {
	snprintf(out, size, "%s %s", name, def ? def->stop_text : "Stops");
}

emote_reaction emote_react(const emote_event* ev, const char* name,
		const emote_state* local) {
	emote_reaction r;
	int active = local->phase == EMOTE_PHASE_ENTERING ||
	             local->phase == EMOTE_PHASE_HELD;

	memset(&r, 0, sizeof(r));
	if(ev->result == EMOTE_RESULT_STARTED) {
		const emote_def* def = emote_lookup(ev->emote);

		if(def) {
			r.say = EMOTE_SAY_EMOTE;
			snprintf(r.text, sizeof(r.text), "%s %s", name, def->verb);
		}
	} else if(ev->result >= EMOTE_REFUSED_UNKNOWN && ev->result <= EMOTE_REFUSED_COMBAT) {
		const emote_def* asked = emote_lookup(ev->requested);

		r.say = EMOTE_SAY_ERROR;
		snprintf(r.text, sizeof(r.text), "Unable to %s",
		         asked ? asked->command : "emote");
		r.end_local = active && local->id == ev->requested;
	} else if(ev->result >= EMOTE_ENDED_MOVED && active) {
		r.say = EMOTE_SAY_EMOTE;
		r.end_local = 1;
		emote_say_stop(r.text, sizeof(r.text), name, emote_lookup(local->id));
	}

	return r;
}
