// Lean, upper-body twist and head turn from how a body is moving.

#include "render/character_procedural.h"
#include "render/character_tuning.h"

#include <math.h>

#define PI_F 3.14159265359f

static float deg(float d) {
	return d * PI_F / 180.0f;
}

// Wrap an angle into (-pi, pi]
static float wrap_angle(float a) {
	while(a > PI_F)
		a -= 2.0f * PI_F;
	while(a <= -PI_F)
		a += 2.0f * PI_F;

	return a;
}

static float clamp_to(float v, float lo, float hi) {
	if(v < lo)
		return lo;
	if(v > hi)
		return hi;

	return v;
}

void character_procedural_reset(character_procedural* p, float yaw) {
	character_procedural fresh = { 0 };

	fresh.last_yaw = yaw;
	*p = fresh;
}

// Lean from how fast the body goes, speeds up or slows down, and corners
static void lean_targets(character_offsets* want, float speed, float yaw_rate,
                         float accel) {
	float steady = speed / CHARACTER_RUN_CLIP_SPEED *
	               deg(CHARACTER_LEAN_FWD_AT_RUN_DEG);
	float push = accel / 1000.0f * deg(CHARACTER_LEAN_ACCEL_DEG_PER_K);
	float side = speed * yaw_rate / 1000.0f * deg(CHARACTER_LEAN_SIDE_DEG_PER_K);
	float side_max = deg(CHARACTER_LEAN_SIDE_MAX_DEG);

	want->lean_forward = clamp_to(steady + push, -deg(CHARACTER_LEAN_BACK_MAX_DEG),
	                              deg(CHARACTER_LEAN_FWD_MAX_DEG));
	want->lean_left = clamp_to(side, -side_max, side_max);
}

// Split a turn toward `err` radians between the spine and the head
static void split_turn(character_offsets* want, float err, float spine_share,
                       float spine_max) {
	float head_max = deg(CHARACTER_HEAD_MAX_DEG);

	want->spine_twist = clamp_to(err * spine_share, -spine_max, spine_max);
	want->head_yaw = clamp_to(err - want->spine_twist, -head_max, head_max);
}

// The upper body leads a turn; an aim point or a look target overrides that
static void turn_targets(character_offsets* want,
                         const character_procedural_input* in, float yaw_rate) {
	if(in->has_aim) {
		float aim_max = deg(CHARACTER_AIM_TWIST_MAX_DEG);

		split_turn(want, wrap_angle(in->aim_yaw - in->yaw), 1.0f, aim_max);
	} else if(in->has_look) {
		float look_max = deg(CHARACTER_TWIST_MAX_DEG);

		split_turn(want, wrap_angle(in->look_yaw - in->yaw),
		           CHARACTER_LOOK_TWIST_SHARE, look_max);
	} else {
		float lead = yaw_rate * CHARACTER_HEAD_LEAD_SEC;

		split_turn(want, lead, CHARACTER_TWIST_LEAD_SEC / CHARACTER_HEAD_LEAD_SEC,
		           deg(CHARACTER_TWIST_MAX_DEG));
	}
}

static float chase(float value, float target, float follow) {
	return value + (target - value) * follow;
}

void character_procedural_update(character_procedural* p,
                                 const character_procedural_input* in,
                                 float dt) {
	character_offsets want = { 0 };
	float yaw_rate = 0.0f;
	float accel = 0.0f;
	float follow;
	float rate_max = deg(CHARACTER_PROC_MAX_YAW_RATE_DEG);

	if(dt <= 0.0f)
		return;

	if(p->primed) {
		yaw_rate = clamp_to(wrap_angle(in->yaw - p->last_yaw) / dt, -rate_max,
		                    rate_max);
		accel = (in->speed - p->last_speed) / dt;
	}
	p->accel = chase(p->accel, accel, 1.0f - expf(-CHARACTER_PROC_ACCEL_FOLLOW * dt));
	p->last_yaw = in->yaw;
	p->last_speed = in->speed;
	p->primed = !in->held;

	if(!in->held) {
		lean_targets(&want, in->speed, yaw_rate, p->accel);
		turn_targets(&want, in, yaw_rate);
	}

	follow = 1.0f - expf(-CHARACTER_PROC_FOLLOW * dt);
	p->now.lean_forward = chase(p->now.lean_forward, want.lean_forward, follow);
	p->now.lean_left = chase(p->now.lean_left, want.lean_left, follow);
	p->now.spine_twist = chase(p->now.spine_twist, want.spine_twist, follow);
	p->now.head_yaw = chase(p->now.head_yaw, want.head_yaw, follow);
}
