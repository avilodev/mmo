/**
 * @file
 * Pick, time and blend a body's clip from its drawn movement.
 */

#include "render/character_motion.h"
#include "render/character_tuning.h"

#include <math.h>

#define PI_F 3.14159265359f

/** A jump this far in one update is a teleport or respawn, not movement. */
#define CHARACTER_TELEPORT_DISTANCE 256.0f

void character_motion_reset(CharacterMotion* m, float x, float y, float yaw) {
    m->last_x    = x;
    m->last_y    = y;
    m->speed     = 0.0f;
    m->yaw       = yaw;
    m->anim      = CHARACTER_ANIM_IDLE;
    m->prev      = CHARACTER_ANIM_IDLE;
    m->anim_time = 0.0f;
    m->prev_time = 0.0f;
    m->blend     = 1.0f;
}

float character_motion_clip_rate(CharacterAnim anim, float speed) {
    float reference;
    switch (anim) {
        case CHARACTER_ANIM_WALK: reference = CHARACTER_WALK_CLIP_SPEED; break;
        case CHARACTER_ANIM_JOG:  reference = CHARACTER_JOG_CLIP_SPEED;  break;
        case CHARACTER_ANIM_RUN:  reference = CHARACTER_RUN_CLIP_SPEED;  break;
        default:                  return 1.0f;
    }
    float rate = speed / reference;
    if (rate < CHARACTER_CLIP_RATE_MIN) rate = CHARACTER_CLIP_RATE_MIN;
    if (rate > CHARACTER_CLIP_RATE_MAX) rate = CHARACTER_CLIP_RATE_MAX;
    return rate;
}

/** Wrap an angle into (-pi, pi]. */
static float wrap_angle(float a) {
    while (a > PI_F)   a -= 2.0f * PI_F;
    while (a <= -PI_F) a += 2.0f * PI_F;
    return a;
}

/** Turn `yaw` toward `target` by at most `max_step` radians. */
static float turn_toward(float yaw, float target, float max_step) {
    float diff = wrap_angle(target - yaw);
    if (diff >  max_step) diff =  max_step;
    if (diff < -max_step) diff = -max_step;
    return wrap_angle(yaw + diff);
}

static float advance(float time, float dt, float duration) {
    time += dt;
    if (duration > 0.0f) {
        time = fmodf(time, duration);
        if (time < 0.0f) time += duration;
    }
    return time;
}

static int is_gait(CharacterAnim a) {
    return a == CHARACTER_ANIM_WALK || a == CHARACTER_ANIM_JOG || a == CHARACTER_ANIM_RUN;
}

static CharacterAnim choose(float speed, const CharacterHints* hints) {
    if (speed >= CHARACTER_RUN_THRESHOLD)  return CHARACTER_ANIM_RUN;
    if (speed >= CHARACTER_JOG_THRESHOLD)  return CHARACTER_ANIM_JOG;
    if (speed >= CHARACTER_MOVE_THRESHOLD) return CHARACTER_ANIM_WALK;
    if (hints && hints->pivoting)          return CHARACTER_ANIM_WALK;
    if (hints && hints->talking)           return CHARACTER_ANIM_TALK;
    return CHARACTER_ANIM_IDLE;
}

void character_motion_update(CharacterMotion* m, float x, float y, float dt,
                             const CharacterHints* hints,
                             const float durations[CHARACTER_ANIM_COUNT]) {
    if (dt <= 0.0f) return;

    float dx = x - m->last_x;
    float dy = y - m->last_y;
    float step = sqrtf(dx * dx + dy * dy);
    m->last_x = x;
    m->last_y = y;

    /* A respawn or a server snap moves the body without it walking there. */
    if (step > CHARACTER_TELEPORT_DISTANCE) {
        step = 0.0f;
        m->speed = 0.0f;
    }

    /* Positions of remote bodies arrive in 20 Hz steps and are interpolated,
     * so the raw per-frame speed flickers; the body should not. */
    float follow = 1.0f - expf(-CHARACTER_SPEED_SMOOTHING * dt);
    m->speed += (step / dt - m->speed) * follow;

    float max_turn = CHARACTER_TURN_DEG_PER_SEC * PI_F / 180.0f * dt;
    if (hints && hints->has_facing) {
        m->yaw = wrap_angle(hints->facing_yaw);
    } else if (step > 1e-3f && m->speed >= CHARACTER_MOVE_THRESHOLD) {
        m->yaw = turn_toward(m->yaw, atan2f(dx, dy), max_turn);
    } else if (hints && hints->has_look) {
        float lx = hints->look_x - x, ly = hints->look_y - y;
        if (lx * lx + ly * ly > 1.0f)
            m->yaw = turn_toward(m->yaw, atan2f(lx, ly), max_turn * 0.5f);
    }

    CharacterAnim want = choose(m->speed, hints);
    if (want != m->anim) {
        /* Walk, jog and sprint are one gait at three speeds: keep the phase
         * so the feet do not restart mid-stride. */
        int same_gait = is_gait(want) && is_gait(m->anim);
        float phase = (durations[m->anim] > 0.0f) ? m->anim_time / durations[m->anim] : 0.0f;

        m->prev      = m->anim;
        m->prev_time = m->anim_time;
        m->anim      = want;
        m->anim_time = same_gait ? phase * durations[want] : 0.0f;
        m->blend     = 0.0f;
    }

    /* A pivot steps round at the walk's own pace, not a crawl's. */
    int stepping_round = hints && hints->pivoting && m->speed < CHARACTER_MOVE_THRESHOLD;
    float rate = stepping_round ? 1.0f : character_motion_clip_rate(m->anim, m->speed);
    m->anim_time = advance(m->anim_time, dt * rate,
                           durations[m->anim]);
    if (m->blend < 1.0f) {
        m->prev_time = advance(m->prev_time, dt * character_motion_clip_rate(m->prev, m->speed),
                               durations[m->prev]);
        m->blend += dt / CHARACTER_BLEND_TIME;
        if (m->blend > 1.0f) m->blend = 1.0f;
    }
}
