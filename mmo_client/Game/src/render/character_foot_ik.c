// Two-bone leg solver and the pelvis rule for stairs and slopes.

#include "render/character_foot_ik.h"

#include <math.h>

// Bones or lines shorter than this are degenerate; leave the clip's leg
#define MIN_LENGTH 1.0e-6f
// A knee offset smaller than this share of the thigh is treated as straight
#define MIN_POLE 0.02f
// The leg never folds closer than this share of its length
#define MIN_FOLD 0.02f

static void sub3(const float a[3], const float b[3], float out[3]) {
	for(int i = 0; i < 3; i++)
		out[i] = a[i] - b[i];
}

static float dot3(const float a[3], const float b[3]) {
	return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static float length3(const float a[3]) {
	return sqrtf(dot3(a, a));
}

static void copy3(const float from[3], float to[3]) {
	for(int i = 0; i < 3; i++)
		to[i] = from[i];
}

// The unit direction of v, or 0 and false when v is too short to have one
static int normalize3(const float v[3], float out[3]) {
	float length = length3(v);

	if(length < MIN_LENGTH)
		return 0;
	for(int i = 0; i < 3; i++)
		out[i] = v[i] / length;

	return 1;
}

// v with its part along `axis` removed, made unit; false if nothing is left
static int perpendicular(const float v[3], const float axis[3], float out[3]) {
	float along = dot3(v, axis);
	float flat[3];

	for(int i = 0; i < 3; i++)
		flat[i] = v[i] - axis[i] * along;
	if(length3(flat) < MIN_POLE * length3(v))
		return 0;

	return normalize3(flat, out);
}

// The clip's knee direction, square to the hip-ankle line
static void clip_pole(const foot_ik_leg* clip, const float thigh[3], float pole[3]) {
	static const float FORWARD[3] = { 0.0f, 0.0f, 1.0f };
	float line[3];
	float unit[3];

	sub3(clip->ankle, clip->hip, line);
	if(normalize3(line, unit) && perpendicular(thigh, unit, pole))
		return;
	copy3(FORWARD, pole);
}

float foot_ik_pelvis_target(float raise_a, float raise_b, float max_drop,
                            float max_rise) {
	float low;

	if(!isfinite(raise_a) && !isfinite(raise_b))
		return 0.0f;
	if(!isfinite(raise_a))
		low = raise_b;
	else if(!isfinite(raise_b))
		low = raise_a;
	else
		low = raise_a < raise_b ? raise_a : raise_b;
	if(low < -max_drop)
		low = -max_drop;
	if(low > max_rise)
		low = max_rise;

	return low;
}

float foot_ik_chase(float value, float target, float rate, float dt) {
	if(!isfinite(target))
		return isfinite(value) ? value : 0.0f;
	if(!isfinite(value))
		return target;
	if(!(dt > 0.0f) || !(rate > 0.0f))
		return value;

	return value + (target - value) * (1.0f - expf(-rate * dt));
}

void foot_ik_solve_leg(const foot_ik_leg* clip, float raise, float max_reach,
                       foot_ik_result* out) {
	static const float SIDE[3] = { 1.0f, 0.0f, 0.0f };
	float thigh[3];
	float shin[3];
	float reach[3];
	float axis[3];
	float pole[3];
	float l1;
	float l2;
	float full;
	float own;
	float limit;
	float dist;
	float along;
	float bend;

	copy3(clip->knee, out->knee);
	copy3(clip->ankle, out->ankle);
	if(!isfinite(raise))
		return;

	sub3(clip->knee, clip->hip, thigh);
	sub3(clip->ankle, clip->knee, shin);
	sub3(clip->ankle, clip->hip, reach);
	own = length3(reach);
	reach[1] += raise;
	l1 = length3(thigh);
	l2 = length3(shin);
	full = length3(reach);
	if(l1 < MIN_LENGTH || l2 < MIN_LENGTH || !normalize3(reach, axis))
		return;

	// never reach less than the clip does, or flat ground bends a straight leg
	limit = max_reach * (l1 + l2);
	if(limit < own)
		limit = own;
	dist = full;
	if(dist > limit)
		dist = limit;
	if(dist < fabsf(l1 - l2) + MIN_FOLD * (l1 + l2))
		dist = fabsf(l1 - l2) + MIN_FOLD * (l1 + l2);

	// the knee is `along` down the line to the ankle and `bend` off to the side
	clip_pole(clip, thigh, pole);
	if(!perpendicular(pole, axis, pole) && !perpendicular(SIDE, axis, pole))
		return;
	
	along = (l1 * l1 - l2 * l2 + dist * dist) / (2.0f * dist);
	bend = sqrtf(fmaxf(0.0f, l1 * l1 - along * along));
	for(int i = 0; i < 3; i++) {
		out->knee[i] = clip->hip[i] + axis[i] * along + pole[i] * bend;
		out->ankle[i] = clip->hip[i] + axis[i] * dist;
	}
}
