#ifndef MOVE_VALIDATOR_H
#define MOVE_VALIDATOR_H

/**
 * @file
 * Declare server-authoritative movement limits and verdicts.
 */

#include "server_types.h"
#include "world_motion.h"

/** Multiplier allowed above the server-side movement speed. */
#define MOVE_SPEED_TOLERANCE      1.5f

/** Maximum movement credit accumulated during idle time, in seconds. */
#define MOVE_BUDGET_BURST_SECONDS 1.0f

/** Player collision-box half-extent in world pixels. Must equal the client's
 *  PLAYER_HALF_SIZE_PX (player.h): under a tile, so a character fits a
 *  doorway the city collision leaves one tile wide. */
#define MOVE_PLAYER_HALF_SIZE     6.0f

/** Movement speed used when server stats provide no positive finite value. */
#define MOVE_DEFAULT_SPEED        200.0f

// How long stale moves are dropped before the client's echo is trusted
#define MOVE_STALE_SECONDS        0.75f

/** Identify acceptance or the first movement rejection condition. */
typedef enum {
    MOVE_ACCEPT = 0,
    MOVE_REJECT_COORD,      /**< Coordinate is non-finite or outside the loaded world. */
    MOVE_REJECT_SPEED,      /**< Travel distance exceeds accrued credit. */
    MOVE_REJECT_COLLISION,  /**< Travel path crosses solid geometry. */
    MOVE_REJECT_HEIGHT,     /**< Height does not match the replayed simulation. */
} MoveVerdict;

MoveVerdict move_validate(MoveBudget* budget,
                          float from_x, float from_y,
                          float to_x,   float to_y,
                          float to_z,   int jump,
                          float move_speed,
                          const struct timespec* now,
                          vertical_state* vs);

// After a refusal the body falls at (x, y) for the time the move spent
void move_hold(MoveBudget* budget, float x, float y, vertical_state* vs);

// 1: a move sent before the last correction, held at (x, y); healing on timeout
int move_drop_stale(MoveBudget* budget, uint32_t echo, float x, float y,
                    float move_speed, const struct timespec* now,
                    vertical_state* vs);

const char* move_verdict_name(MoveVerdict verdict);

#endif // MOVE_VALIDATOR_H
