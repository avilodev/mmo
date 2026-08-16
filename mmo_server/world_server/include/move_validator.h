#ifndef MOVE_VALIDATOR_H
#define MOVE_VALIDATOR_H

// Movement validation, separated from the packet handler so the decision can be
// tested without sockets, locks, or a database.
//
// The client is authoritative over nothing here. It proposes a position; the
// server decides whether the player could have reached it, and every input to
// that decision — the coordinates, the elapsed time, the move speed — is
// treated as suspect and clamped or rejected before it is used.

#include "server_types.h"

// Sustained speed a client may exceed its server-side move_speed by. The burst
// allowance below absorbs lag spikes, so this only has to cover the ordinary
// drift between the client's simulation and the server's.
#define MOVE_SPEED_TOLERANCE      1.5f

// Seconds of travel a client may bank while it is not moving. This is what
// lets a player who stalled briefly catch up in one packet, and it is also the
// hard ceiling on how far any burst of queued packets can carry them.
#define MOVE_BUDGET_BURST_SECONDS 1.0f

// Half-extent of the player's collision box, in world pixels.
#define MOVE_PLAYER_HALF_SIZE     16.0f

// Used when a character's move_speed is missing or unusable.
#define MOVE_DEFAULT_SPEED        200.0f

typedef enum {
    MOVE_ACCEPT = 0,
    MOVE_REJECT_COORD,      // non-finite, or outside the loaded world
    MOVE_REJECT_SPEED,      // farther than the accrued budget allows
    MOVE_REJECT_COLLISION,  // the path crosses solid geometry
} MoveVerdict;

// Start a character with no banked credit. Called when a character is loaded,
// so a slow login cannot accrue a large allowance before the first move.
static inline void move_budget_reset(MoveBudget* budget,
                                     const struct timespec* now) {
    budget->credit  = 0.0f;
    budget->last_tv = *now;
}

// Decide whether a character at (from_x, from_y) may move to (to_x, to_y).
//
// The budget is always advanced, including on rejection: the time really did
// pass, and not consuming it would let a client alternate rejected and
// accepted moves to collect the same interval twice. Credit is only *spent* by
// an accepted move.
MoveVerdict move_validate(MoveBudget* budget,
                          float from_x, float from_y,
                          float to_x,   float to_y,
                          float move_speed,
                          const struct timespec* now);

// Stable short name for logs.
const char* move_verdict_name(MoveVerdict verdict);

#endif // MOVE_VALIDATOR_H
