/**
 * @file
 * Validate client movement proposals against server limits and collision data.
 */

#include "move_validator.h"
#include "world_collision.h"

#include <math.h>

/**
 * Return the stable log name for a movement verdict.
 *
 * @return      A static verdict name, or "unknown" for an unrecognized value.
 */
const char* move_verdict_name(MoveVerdict verdict) {
    switch (verdict) {
        case MOVE_ACCEPT:           return "accept";
        case MOVE_REJECT_COORD:     return "coord";
        case MOVE_REJECT_SPEED:     return "speed";
        case MOVE_REJECT_COLLISION: return "collision";
        default:                    return "unknown";
    }
}

/**
 * Validate a proposed position and debit its accepted travel distance.
 *
 * The time budget advances on every verdict, while distance credit is spent only on acceptance.
 *
 * @param budget  Mutable movement credit and timestamp state.
 * @param from_x  Current server x-coordinate in world units.
 * @param from_y  Current server y-coordinate in world units.
 * @param to_x  Proposed x-coordinate in world units.
 * @param to_y  Proposed y-coordinate in world units.
 * @param move_speed  Server-side movement speed in world units per second.
 * @param now  Current monotonic timestamp.
 * @return      MOVE_ACCEPT or the first applicable rejection verdict.
 */
MoveVerdict move_validate(MoveBudget* budget,
                          float from_x, float from_y,
                          float to_x,   float to_y,
                          float move_speed,
                          const struct timespec* now) {
    // reject coordinates before distance arithmetic
    if (!world_coord_is_valid(to_x, to_y))
        return MOVE_REJECT_COORD;

    float speed = (isfinite(move_speed) && move_speed > 0.0f)
                ? move_speed : MOVE_DEFAULT_SPEED;
    float max_speed = speed * MOVE_SPEED_TOLERANCE;

    double elapsed = (double)(now->tv_sec  - budget->last_tv.tv_sec)
                   + (double)(now->tv_nsec - budget->last_tv.tv_nsec) / 1e9;

    // limit credit earned during idle intervals
    if (!(elapsed > 0.0)) elapsed = 0.0;
    if (elapsed > (double)MOVE_BUDGET_BURST_SECONDS)
        elapsed = (double)MOVE_BUDGET_BURST_SECONDS;

    float ceiling = max_speed * MOVE_BUDGET_BURST_SECONDS;
    float credit  = budget->credit + max_speed * (float)elapsed;
    if (credit > ceiling) credit = ceiling;

    // consume elapsed time for every verdict
    budget->last_tv = *now;
    budget->credit  = credit;

    float dx = to_x - from_x;
    float dy = to_y - from_y;
    float distance = sqrtf(dx * dx + dy * dy);

    if (!(distance <= credit))
        return MOVE_REJECT_SPEED;

    if (world_collision_check_box_path(from_x, from_y, to_x, to_y,
                                       MOVE_PLAYER_HALF_SIZE))
        return MOVE_REJECT_COLLISION;

    budget->credit = credit - distance;
    return MOVE_ACCEPT;
}
