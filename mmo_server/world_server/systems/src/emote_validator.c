// Decide whether an emote may start, and when a running one must end.

#include "emote_validator.h"
#include "world_seats.h"

#include <math.h>

typedef struct {
	int known;
	int needs_seat;
	int arg_limit; // 0 takes any arg; else the arg must be below this
} emote_rule;

// An emote missing from this table is unknown.
static const emote_rule RULES[EMOTE_COUNT] = {
	[EMOTE_SIT] = { .known = 1, .needs_seat = 1 },
	[EMOTE_DANCE] = { .known = 1 },
	[EMOTE_CUSTOM] = { .known = 1, .arg_limit = EMOTE_CLIP_COUNT },
	[EMOTE_LEAN] = { .known = 1 },
	[EMOTE_LEAN_FOOT] = { .known = 1 },
};

// The seat must exist, be within reach of the player, and be free; a
// shopkeeper's seat is never free.
static EmoteResult check_seat(uint8_t seat_index, const emote_actor* who,
		emote_seat_taken_fn seat_taken, void* ctx) {
	const world_seat* seat = world_seats_get(seat_index);
	float reach = WORLD_SEAT_REACH_UNITS + EMOTE_SEAT_REACH_SLACK;

	if(!seat)
		return EMOTE_REFUSED_NO_SEAT;
	if(hypotf(seat->x - who->x, seat->y - who->y) > reach)
		return EMOTE_REFUSED_NO_SEAT;
	if(world_seat_is_reserved(seat))
		return EMOTE_REFUSED_SEAT_TAKEN;
	if(seat_taken && seat_taken(ctx, seat_index))
		return EMOTE_REFUSED_SEAT_TAKEN;

	return EMOTE_RESULT_STARTED;
}

EmoteResult emote_validate(uint8_t emote, uint8_t arg, const emote_actor* who,
		emote_seat_taken_fn seat_taken, void* ctx) {
	if(emote == EMOTE_NONE)
		return EMOTE_RESULT_STOPPED;
	if(emote >= EMOTE_COUNT || !RULES[emote].known)
		return EMOTE_REFUSED_UNKNOWN;
	if(RULES[emote].arg_limit && arg >= RULES[emote].arg_limit)
		return EMOTE_REFUSED_UNKNOWN;
	if(who->dead)
		return EMOTE_REFUSED_DEAD;
	if(who->in_combat)
		return EMOTE_REFUSED_COMBAT;
	if(RULES[emote].needs_seat)
		return check_seat(arg, who, seat_taken, ctx);

	return EMOTE_RESULT_STARTED;
}

EmoteResult emote_upkeep(int dead, double started_at, double last_combat_time) {
	if(dead)
		return EMOTE_ENDED_DEAD;
	if(last_combat_time > started_at)
		return EMOTE_ENDED_COMBAT;

	return EMOTE_RESULT_STARTED;
}
