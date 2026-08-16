#include "move_validator.h"
#include "world_collision.h"

#include <math.h>

const char* move_verdict_name(MoveVerdict verdict) {
    switch (verdict) {
        case MOVE_ACCEPT:           return "accept";
        case MOVE_REJECT_COORD:     return "coord";
        case MOVE_REJECT_SPEED:     return "speed";
        case MOVE_REJECT_COLLISION: return "collision";
        default:                    return "unknown";
    }
}

MoveVerdict move_validate(MoveBudget* budget,
                          float from_x, float from_y,
                          float to_x,   float to_y,
                          float move_speed,
                          const struct timespec* now) {
    // Coordinates first. Nothing downstream — the distance maths, the collision
    // sampling, the eventual database write — behaves sanely on a non-finite
    // value, and a NaN in particular passes every `>` test it meets. This is
    // repeated at the router; validating again here means a future caller
    // cannot reach the rest of this function with garbage.
    if (!world_coord_is_valid(to_x, to_y))
        return MOVE_REJECT_COORD;

    // move_speed comes from server-side stats, not the packet, but it is
    // derived from gear and buffs and so is worth bounds-checking anyway.
    float speed = (isfinite(move_speed) && move_speed > 0.0f)
                ? move_speed : MOVE_DEFAULT_SPEED;
    float max_speed = speed * MOVE_SPEED_TOLERANCE;

    double elapsed = (double)(now->tv_sec  - budget->last_tv.tv_sec)
                   + (double)(now->tv_nsec - budget->last_tv.tv_nsec) / 1e9;

    // Clamped at both ends. Negative would mean the monotonic clock ran
    // backwards; a very large value means either a zeroed last_tv or a long
    // idle period, neither of which should hand out a large allowance. Written
    // as !(elapsed > 0.0) so a NaN elapsed clamps to zero rather than through.
    if (!(elapsed > 0.0)) elapsed = 0.0;
    if (elapsed > (double)MOVE_BUDGET_BURST_SECONDS)
        elapsed = (double)MOVE_BUDGET_BURST_SECONDS;

    float ceiling = max_speed * MOVE_BUDGET_BURST_SECONDS;
    float credit  = budget->credit + max_speed * (float)elapsed;
    if (credit > ceiling) credit = ceiling;

    // Time is consumed whatever the verdict.
    budget->last_tv = *now;
    budget->credit  = credit;

    float dx = to_x - from_x;
    float dy = to_y - from_y;
    float distance = sqrtf(dx * dx + dy * dy);

    // from_* is server state and to_* is validated above, so distance is
    // finite here; the negated form keeps that true if either ever stops being.
    if (!(distance <= credit))
        return MOVE_REJECT_SPEED;

    if (world_collision_check_box_path(from_x, from_y, to_x, to_y,
                                       MOVE_PLAYER_HALF_SIZE))
        return MOVE_REJECT_COLLISION;

    budget->credit = credit - distance;
    return MOVE_ACCEPT;
}
