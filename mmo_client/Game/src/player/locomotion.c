/**
 * @file
 * Turn key direction into GTA-style body movement.
 */

#include "player/locomotion.h"

#include <math.h>

#define PI_F 3.14159265359f

static float deg(float d) { return d * PI_F / 180.0f; }

/** Wrap an angle into (-pi, pi]. */
static float wrap_angle(float a) {
    while (a > PI_F)   a -= 2.0f * PI_F;
    while (a <= -PI_F) a += 2.0f * PI_F;
    return a;
}

static float approach(float value, float target, float step) {
    if (value < target) return (value + step > target) ? target : value + step;
    return (value - step < target) ? target : value - step;
}

void locomotion_halt(Locomotion* l) {
    l->speed = 0.0f;
    l->pivoting = 0;
}

float locomotion_mode_speed(LocoMode mode, float max_speed) {
    switch (mode) {
        case LOCO_MODE_WALK:   return max_speed * LOCO_WALK_FRACTION;
        case LOCO_MODE_SPRINT: return max_speed * LOCO_SPRINT_FRACTION;
        default:               return max_speed * LOCO_RUN_FRACTION;
    }
}

void locomotion_step(Locomotion* l, float in_x, float in_y, LocoMode mode,
                     float max_speed, float dt, float* out_dx, float* out_dy) {
    *out_dx = *out_dy = 0.0f;
    if (dt <= 0.0f) return;

    float walk_speed = locomotion_mode_speed(LOCO_MODE_WALK, max_speed);
    float target_speed = 0.0f;

    if (in_x * in_x + in_y * in_y > 1e-6f) {
        float diff = wrap_angle(atan2f(in_x, in_y) - l->heading);

        /* Asked to go the other way from a standstill: turn on the spot
         * before stepping off. At speed the same request first brakes (the
         * alignment term below), and pivots once slow enough. */
        if (!l->pivoting && fabsf(diff) > deg(LOCO_PIVOT_START_DEG) &&
            l->speed <= LOCO_PIVOT_SPEED)
            l->pivoting = 1;

        float rate = l->pivoting                  ? LOCO_PIVOT_TURN_DEG
                   : (l->speed > walk_speed * 1.5f) ? LOCO_RUN_TURN_DEG
                                                   : LOCO_WALK_TURN_DEG;
        float step = deg(rate) * dt;
        if (diff >  step) diff =  step;
        if (diff < -step) diff = -step;
        l->heading = wrap_angle(l->heading + diff);

        float left = fabsf(wrap_angle(atan2f(in_x, in_y) - l->heading));
        if (l->pivoting && left < deg(LOCO_PIVOT_END_DEG)) l->pivoting = 0;

        /* The body moves the way it faces, and only as fast as that is the
         * way it was asked to go: facing away from the keys, it brakes. */
        float align = cosf(left);
        if (align < 0.0f) align = 0.0f;
        target_speed = l->pivoting ? 0.0f : locomotion_mode_speed(mode, max_speed) * align;
    } else {
        l->pivoting = 0;
    }

    float rate = (target_speed > l->speed) ? LOCO_ACCEL : LOCO_DECEL;
    l->speed = approach(l->speed, target_speed, rate * dt);
    if (l->speed > max_speed) l->speed = max_speed;   /* the server's ceiling */

    *out_dx = sinf(l->heading) * l->speed * dt;
    *out_dy = cosf(l->heading) * l->speed * dt;
}
