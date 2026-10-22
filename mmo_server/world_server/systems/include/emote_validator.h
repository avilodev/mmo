#ifndef EMOTE_VALIDATOR_H
#define EMOTE_VALIDATOR_H

// The server's emote rules. To add one: EmoteId, a RULES row, a client row.

#include "protocol.h"

// How far past the seat reach a sit is still accepted (our position trails).
#define EMOTE_SEAT_REACH_SLACK 24.0f

// What the server knows about the player asking.
typedef struct {
	float x, y;
	int dead;
	int in_combat;
} emote_actor;

// Whether another player holds the seat. NULL means nobody does.
typedef int (*emote_seat_taken_fn)(void* ctx, int seat);

// Returns STARTED, STOPPED (always allowed) or an EMOTE_REFUSED_* reason.
EmoteResult emote_validate(uint8_t emote, uint8_t arg, const emote_actor* who,
		emote_seat_taken_fn seat_taken, void* ctx);

// Returns STARTED to let a running emote run, or the EMOTE_ENDED_* cause.
EmoteResult emote_upkeep(int dead, double started_at, double last_combat_time);

#endif /* EMOTE_VALIDATOR_H */
