#ifndef MOVE_VALIDATOR_H
#define MOVE_VALIDATOR_H

/**
 * @file
 * Declare server-authoritative movement limits and verdicts.
 */

#include "server_types.h"

/** Multiplier allowed above the server-side movement speed. */
#define MOVE_SPEED_TOLERANCE      1.5f

/** Maximum movement credit accumulated during idle time, in seconds. */
#define MOVE_BUDGET_BURST_SECONDS 1.0f

/** Player collision-box half-extent in world pixels. */
#define MOVE_PLAYER_HALF_SIZE     16.0f

/** Movement speed used when server stats provide no positive finite value. */
#define MOVE_DEFAULT_SPEED        200.0f

/** Identify acceptance or the first movement rejection condition. */
typedef enum {
    MOVE_ACCEPT = 0,
    MOVE_REJECT_COORD,      /**< Coordinate is non-finite or outside the loaded world. */
    MOVE_REJECT_SPEED,      /**< Travel distance exceeds accrued credit. */
    MOVE_REJECT_COLLISION,  /**< Travel path crosses solid geometry. */
} MoveVerdict;

/**
 * Reset movement credit at the supplied monotonic time.
 *
 * @param budget  Movement budget to initialize.
 * @param now  Monotonic timestamp used for the next accrual interval.
 */
static inline void move_budget_reset(MoveBudget* budget,
                                     const struct timespec* now) {
    budget->credit  = 0.0f;
    budget->last_tv = *now;
}

MoveVerdict move_validate(MoveBudget* budget,
                          float from_x, float from_y,
                          float to_x,   float to_y,
                          float move_speed,
                          const struct timespec* now);

const char* move_verdict_name(MoveVerdict verdict);

#endif // MOVE_VALIDATOR_H
