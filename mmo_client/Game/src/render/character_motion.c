/**
 * @file
 * Pick, time and blend a body's clip from its drawn movement.
 */

#include "render/character_motion.h"
#include "render/character_tuning.h"
#include "protocol.h"

#include <math.h>

#define PI_F 3.14159265359f

/** A jump this far in one update is a teleport or respawn, not movement. */
#define CHARACTER_TELEPORT_DISTANCE 256.0f

// Each body draws its own random stream; this makes the seeds differ
static unsigned int g_seed = 2463534242u;

// A random fraction in [0, 1) from the body's own stream (xorshift32)
static float next_fraction(CharacterMotion* m) {
	m->rng ^= m->rng << 13;
	m->rng ^= m->rng >> 17;
	m->rng ^= m->rng << 5;

	return (float)(m->rng >> 8) / 16777216.0f;
}

// Start the wait for the next fidget, anywhere in the allowed range
static void schedule_fidget(CharacterMotion* m) {
	float range = CHARACTER_FIDGET_MAX_SEC - CHARACTER_FIDGET_MIN_SEC;

	m->idle_time = 0.0f;
	m->fidget_time = 0.0f;
	m->fidgeting = 0;
	m->fidget_after = CHARACTER_FIDGET_MIN_SEC + range * next_fraction(m);
}

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
	character_procedural_reset(&m->proc, yaw);
	g_seed = g_seed * 1664525u + 1013904223u;
	m->rng = g_seed ? g_seed : 1u;
	m->pivot_dir = 0;
	m->remote_pivoting = 0;
	m->heading_known = 0;
	m->started = 0;
	m->fidget_kind = -1;
	schedule_fidget(m);
}

float character_motion_clip_rate(CharacterAnim anim, float speed) {
    float reference;
    switch (anim) {
        case CHARACTER_ANIM_WALK: 
			reference = CHARACTER_WALK_CLIP_SPEED; 
			break;
        case CHARACTER_ANIM_JOG:  
			reference = CHARACTER_JOG_CLIP_SPEED;  
			break;
        case CHARACTER_ANIM_RUN:  
			reference = CHARACTER_RUN_CLIP_SPEED;  
			break;
        default:                  
			return 1.0f;
    }
    float rate = speed / reference;
    if (rate < CHARACTER_CLIP_RATE_MIN) 
		rate = CHARACTER_CLIP_RATE_MIN;
    if (rate > CHARACTER_CLIP_RATE_MAX) 
		rate = CHARACTER_CLIP_RATE_MAX;

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
    if (diff >  max_step) 
		diff =  max_step;
    if (diff < -max_step) 
		diff = -max_step;

    return wrap_angle(yaw + diff);
}

/** Clips that play once and hold their last frame, rather than looping. */
static int is_one_shot(CharacterAnim a) {
	return a == CHARACTER_ANIM_SIT_ENTER || a == CHARACTER_ANIM_SIT_EXIT ||
	       a == CHARACTER_ANIM_LEAN_ENTER || a == CHARACTER_ANIM_LEAN_EXIT ||
	       a == CHARACTER_ANIM_LEAN_FOOT_ENTER || a == CHARACTER_ANIM_LEAN_FOOT_EXIT ||
	       a == CHARACTER_ANIM_CUSTOM_ENTER || a == CHARACTER_ANIM_CUSTOM_EXIT;
}

static float advance(CharacterAnim anim, float time, float dt, float duration) {
	time += dt;
	if(duration > 0.0f) {
		if(is_one_shot(anim)) {
			if(time > duration)
				time = duration;
		} else {
			time = fmodf(time, duration);
			if(time < 0.0f)
				time += duration;
		}
	}

	return time;
}

static int is_gait(CharacterAnim a) {
    return a == CHARACTER_ANIM_WALK || a == CHARACTER_ANIM_JOG || a == CHARACTER_ANIM_RUN;
}

// The idle variations by kind; the kind is the number sent to other players, so
// add to the end. Cycles: how many times the clip plays before the body settles.
static const struct {
	CharacterAnim anim;
	int cycles;
} FIDGETS[] = {
	{ CHARACTER_ANIM_FIDGET, CHARACTER_FIDGET_CYCLES },
	{ CHARACTER_ANIM_FIDGET_GLANCE, 1 },
	{ CHARACTER_ANIM_FIDGET_KNUCKLES, 1 },
	{ CHARACTER_ANIM_FIDGET_SLEEVE, 1 },
};

#define FIDGET_KINDS ((int)(sizeof(FIDGETS) / sizeof(FIDGETS[0])))

_Static_assert(FIDGET_KINDS <= (MOVE_FIDGET_KIND_MASK >> MOVE_FIDGET_KIND_SHIFT) + 1,
		"more idle variations than MOVE_FIDGET_KIND_MASK can name");

// A kind's clip; an unknown kind, or one whose clip is missing, plays the first
static CharacterAnim fidget_anim(int kind, const float durations[CHARACTER_ANIM_COUNT]) {
	if(kind < 0 || kind >= FIDGET_KINDS || durations[FIDGETS[kind].anim] <= 0.0f)
		return CHARACTER_ANIM_FIDGET;

	return FIDGETS[kind].anim;
}

// The next variation: any with a clip, not the last one played when there is a
// choice. -1 when none has a clip. Draws from the stream only when it must choose.
static int pick_fidget(CharacterMotion* m, const float durations[CHARACTER_ANIM_COUNT]) {
	int kinds[FIDGET_KINDS];
	int n = 0;
	int pick;

	for(int k = 0; k < FIDGET_KINDS; k++)
		if(durations[FIDGETS[k].anim] > 0.0f && k != m->fidget_kind)
			kinds[n++] = k;

	if(n == 0)
		return m->fidget_kind >= 0 && m->fidget_kind < FIDGET_KINDS &&
		       durations[FIDGETS[m->fidget_kind].anim] > 0.0f ? m->fidget_kind : -1;
	if(n == 1)
		return kinds[0];

	pick = (int)(next_fraction(m) * (float)n);

	return kinds[pick < n ? pick : n - 1];
}

// Idle, or an idle variation once the body has stood about long enough
static CharacterAnim idle_or_fidget(CharacterMotion* m, const CharacterHints* hints,
                                    float dt, const float durations[CHARACTER_ANIM_COUNT]) {
	int any = 0;

	for(int k = 0; k < FIDGET_KINDS; k++)
		any |= durations[FIDGETS[k].anim] > 0.0f;

	// Another player's idle variation plays when its owner's does, not on a timer of ours
	if(hints && hints->has_idle_state) {
		CharacterAnim anim = fidget_anim(hints->idle_fidget - 1, durations);

		m->idle_time = 0.0f;
		m->fidgeting = hints->idle_fidget > 0 && durations[anim] > 0.0f;
		if(m->fidgeting)
			m->fidget_kind = hints->idle_fidget - 1;

		return m->fidgeting ? anim : CHARACTER_ANIM_IDLE;
	}

	// Turning on the spot or aiming is not standing about
	if((hints && (hints->pivoting || hints->has_aim)) || !any) {
		m->idle_time = 0.0f;
		m->fidgeting = 0;

		return CHARACTER_ANIM_IDLE;
	}

	if(m->fidgeting) {
		CharacterAnim anim = fidget_anim(m->fidget_kind, durations);
		int cycles = m->fidget_kind >= 0 && m->fidget_kind < FIDGET_KINDS ? FIDGETS[m->fidget_kind].cycles : 1;

		m->fidget_time += dt;
		if(m->fidget_time >= durations[anim] * (float)cycles) {
			schedule_fidget(m);

			return CHARACTER_ANIM_IDLE;
		}

		return anim;
	}

	m->idle_time += dt;
	if(m->idle_time >= m->fidget_after) {
		int kind = pick_fidget(m, durations);

		if(kind < 0)
			return CHARACTER_ANIM_IDLE;

		m->fidget_kind = kind;
		m->fidgeting = 1;
		m->fidget_time = 0.0f;

		return FIDGETS[kind].anim;
	}

	return CHARACTER_ANIM_IDLE;
}

// The clip for turning on the spot, or idle until the turn's direction is known
static CharacterAnim pivot_anim(int dir) {
	if(dir > 0)
		return CHARACTER_ANIM_PIVOT_LEFT;
	if(dir < 0)
		return CHARACTER_ANIM_PIVOT_RIGHT;

	return CHARACTER_ANIM_IDLE;
}

// Which way the body is turning: increasing yaw is a turn to the left. A frame
// with no turn keeps the last direction; a body that is not pivoting has none.
static void track_pivot_dir(CharacterMotion* m, float yaw_step, const CharacterHints* hints) {
	if(!hints || !hints->pivoting) {
		m->pivot_dir = 0;
		return;
	}
	if(yaw_step > CHARACTER_PIVOT_DIR_EPSILON)
		m->pivot_dir = 1;
	else if(yaw_step < -CHARACTER_PIVOT_DIR_EPSILON)
		m->pivot_dir = -1;
}

static CharacterAnim choose(const CharacterMotion* m, const CharacterHints* hints) {
	float speed = m->speed;

	if(hints && hints->has_pose)
		return hints->pose;
	if(hints && hints->pivoting && speed < CHARACTER_JOG_THRESHOLD)
		return pivot_anim(m->pivot_dir);
    if (speed >= CHARACTER_RUN_THRESHOLD)  
		return CHARACTER_ANIM_RUN;
    if (speed >= CHARACTER_JOG_THRESHOLD)  
		return CHARACTER_ANIM_JOG;
    if (speed >= CHARACTER_MOVE_THRESHOLD) 
		return CHARACTER_ANIM_WALK;
    if (hints && hints->talking)           
		return CHARACTER_ANIM_TALK;
    return CHARACTER_ANIM_IDLE;
}

// The heading from (x, y) to a point, or 0 when the point is too close to tell
static int heading_to(float x, float y, float tx, float ty, float* out) {
	float dx = tx - x;
	float dy = ty - y;

	if(dx * dx + dy * dy <= 1.0f)
		return 0;
	*out = atan2f(dx, dy);

	return 1;
}

// Feed the procedural layer this frame's facing, speed and what the body eyes
static void update_procedural(CharacterMotion* m, float x, float y, float dt,
                              const CharacterHints* hints, int held) {
	character_procedural_input in = { 0 };

	in.yaw = m->yaw;
	in.speed = m->speed;
	in.held = held;
	if(hints && hints->has_aim)
		in.has_aim = heading_to(x, y, hints->aim_x, hints->aim_y, &in.aim_yaw);
	if(hints && hints->has_look && m->speed < CHARACTER_MOVE_THRESHOLD)
		in.has_look = heading_to(x, y, hints->look_x, hints->look_y, &in.look_yaw);
	character_procedural_update(&m->proc, &in, dt);
}

// Turn toward the facing the server reports; a wide turn from a stand is a pivot
static void turn_to_reported(CharacterMotion* m, float heading, float max_turn, float dt) {
	float diff = wrap_angle(heading - m->yaw);
	float rate = max_turn;

	// A body that has just come into view stands the way it faces; it does not turn to it
	if(!m->heading_known) {
		m->yaw = heading;
		m->heading_known = 1;

		return;
	}

	if(m->speed > CHARACTER_PIVOT_SPEED)
		m->remote_pivoting = 0;
	else if(!m->remote_pivoting && fabsf(diff) >= CHARACTER_PIVOT_START_DEG * PI_F / 180.0f)
		m->remote_pivoting = 1;

	if(m->remote_pivoting)
		rate = CHARACTER_PIVOT_DEG_PER_SEC * PI_F / 180.0f * dt;
	m->yaw = turn_toward(m->yaw, heading, rate);

	if(m->remote_pivoting &&
	   fabsf(wrap_angle(heading - m->yaw)) < CHARACTER_PIVOT_END_DEG * PI_F / 180.0f)
		m->remote_pivoting = 0;
}

void character_motion_update(CharacterMotion* m, float x, float y, float dt,
                             const CharacterHints* hints,
                             const float durations[CHARACTER_ANIM_COUNT]) {
    if (dt <= 0.0f) 
		return;

    float dx = x - m->last_x;
    float dy = y - m->last_y;
    float step = sqrtf(dx * dx + dy * dy);
    m->last_x = x;
    m->last_y = y;

	// A respawn, a server snap or a seat slide moves the body without walking
	int held = step > CHARACTER_TELEPORT_DISTANCE || (hints && hints->has_pose);

	if(held) {
		step = 0.0f;
		m->speed = 0.0f;
	}

    /* Positions of remote bodies arrive in 20 Hz steps and are interpolated,
     * so the raw per-frame speed flickers; the body should not. */
    float follow = 1.0f - expf(-CHARACTER_SPEED_SMOOTHING * dt);
	if(hints && hints->has_speed && !held)
		m->speed = hints->speed;
	else
		m->speed += (step / dt - m->speed) * follow;

    float max_turn = CHARACTER_TURN_DEG_PER_SEC * PI_F / 180.0f * dt;
	float yaw_before = m->yaw;
    if (hints && hints->has_facing) {
        m->yaw = wrap_angle(hints->facing_yaw);
    } else if (hints && hints->has_heading) {
		turn_to_reported(m, wrap_angle(hints->heading), max_turn, dt);
    } else if (step > 1e-3f && m->speed >= CHARACTER_MOVE_THRESHOLD) {
        m->yaw = turn_toward(m->yaw, atan2f(dx, dy), max_turn);
    } else if (hints && hints->has_look) {
        float lx = hints->look_x - x, ly = hints->look_y - y;
        if (lx * lx + ly * ly > 1.0f)
            m->yaw = turn_toward(m->yaw, atan2f(lx, ly), max_turn * 0.5f);
    }

	// A body turning to a reported facing is pivoting, as the owner's is
	CharacterHints pivot_hints = { 0 };
	const CharacterHints* h = hints;

	if(m->remote_pivoting) {
		if(hints)
			pivot_hints = *hints;
		pivot_hints.pivoting = 1;
		h = &pivot_hints;
	}

	track_pivot_dir(m, wrap_angle(m->yaw - yaw_before), h);
	update_procedural(m, x, y, dt, h, held);

    CharacterAnim want = choose(m, h);

	if(want == CHARACTER_ANIM_IDLE && h && h->still_idle) {
		m->idle_time = 0.0f;
		m->fidgeting = 0;
	} else if(want == CHARACTER_ANIM_IDLE) {
		want = idle_or_fidget(m, h, dt, durations);
	} else {
		m->idle_time = 0.0f;
		m->fidgeting = 0;
	}

	// A body that has just appeared is not changing clip: it starts in the one it wants
	if(!m->started) {
		m->started = 1;
		m->anim = want;
		m->prev = want;
		m->anim_time = 0.0f;
		m->prev_time = 0.0f;
		m->blend = 1.0f;
	}

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

	if(hints && hints->still_idle && m->anim == CHARACTER_ANIM_IDLE)
		m->anim_time = 0.0f;
	else
		m->anim_time = advance(m->anim, m->anim_time,
		                       dt * character_motion_clip_rate(m->anim, m->speed),
		                       durations[m->anim]);
    if (m->blend < 1.0f) {
		m->prev_time = advance(m->prev, m->prev_time,
		                       dt * character_motion_clip_rate(m->prev, m->speed),
		                       durations[m->prev]);
        m->blend += dt / CHARACTER_BLEND_TIME;
        if (m->blend > 1.0f) m->blend = 1.0f;
    }
}
