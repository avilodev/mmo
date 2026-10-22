// Ground under each foot, smoothed into a body lift, foot raises and tilts.

#include "render/character_feet.h"
#include "render/character_foot_ik.h"
#include "render/character_tuning.h"
#include "world/footing.h"
#include "world_spans.h"

#include <math.h>
#include <string.h>

#define PI_F 3.14159265359f
// A scale below this is no body at all
#define MIN_SCALE 1.0e-4f
// Below this weight, with nothing wanted, the effect is off
#define WEIGHT_OFF 0.002f

typedef struct {
	float delta;        // the ground above the body's floor, in rig units
	float normal[3];    // the surface normal in the rig's frame
	float lift;         // how far the foot is off flat ground, world units
	int ok;
} foot_ground;

static float deg(float d) {
	return d * PI_F / 180.0f;
}

static float clamp01(float v) {
	if(v < 0.0f)
		return 0.0f;
	if(v > 1.0f)
		return 1.0f;

	return v;
}

// 1 for a foot on the ground, falling to 0 as it lifts into a swing
static float plant(float lift) {
	return 1.0f - clamp01((lift - FOOT_IK_PLANT_LIFT) /
	                      (FOOT_IK_FREE_LIFT - FOOT_IK_PLANT_LIFT));
}

// The surface normal at a foot, in the rig's frame, from the floor's slope
static void surface_normal(float x, float y, float hint_z, float sine, float cosine,
                           float out[3]) {
	float along = 0.0f;
	float across = 0.0f;
	float gx;
	float gy;
	float nx;
	float nz;
	float length;

	footing_slope_at(x, y, hint_z, sine, cosine, FOOT_IK_SLOPE_REACH, &along, &across);
	// the gradient in the x, y plane: along the heading plus across it
	gx = along * sine - across * cosine;
	gy = along * cosine + across * sine;
	// the normal in GL's frame (up is y), then turned back by the yaw
	nx = -gx;
	nz = -gy;
	out[0] = nx * cosine - nz * sine;
	out[1] = 1.0f;
	out[2] = nx * sine + nz * cosine;
	length = sqrtf(out[0] * out[0] + out[1] * out[1] + out[2] * out[2]);
	for(int i = 0; i < 3; i++)
		out[i] /= length;
}

// Is (x, y) a slope too steep to walk rising above the body's feet at floor_z,
// while the body is not standing on such a slope? A foot is never put there:
// the body cannot climb it.
static int steep_ahead(float x, float y, const character_feet_input* in) {
	float dir_x;
	float dir_y;

	if(!world_spans_steep_above(x, y, in->floor_z))
		return 0;

	return !world_spans_slide(in->x, in->y, in->floor_z, &dir_x, &dir_y, NULL);
}

// The ground under one foot: how far above the body's floor, and its slope
static void read_foot(const character_legs_clip* clip, const character_feet_input* in,
                      int leg, foot_ground* out) {
	const float* ankle = clip->leg[leg].ankle;
	float sine = sinf(in->yaw);
	float cosine = cosf(in->yaw);
	float wx = in->x + in->scale * (ankle[0] * cosine + ankle[2] * sine);
	float wy = in->y + in->scale * (ankle[2] * cosine - ankle[0] * sine);
	float lift = (ankle[1] - in->min_y - in->rest_ankle[leg]) * in->scale;
	float under = 0.0f;
	float ahead = 0.0f;
	float ground;
	float swing;

	out->ok = 0;
	if(!(lift > 0.0f))
		lift = 0.0f;
	out->lift = lift;
	if(!footing_height_at(wx, wy, in->floor_z, &under))
		return;
	if(under > in->floor_z && steep_ahead(wx, wy, in))
		under = in->floor_z;

	// over a drop there is nothing to reach: the foot keeps the clip's height
	if(under < in->floor_z - FOOT_IK_MAX_DROP) {
		out->delta = 0.0f;
		out->normal[0] = 0.0f;
		out->normal[1] = 1.0f;
		out->normal[2] = 0.0f;
		out->ok = 1;

		return;
	}

	// a swinging foot also clears the ground ahead of it
	ground = under;
	swing = 1.0f - plant(lift);
	if(swing > 0.0f &&
	   footing_height_at(wx + sine * FOOT_IK_AHEAD, wy + cosine * FOOT_IK_AHEAD, under,
	                     &ahead) &&
	   ahead > under && !steep_ahead(wx + sine * FOOT_IK_AHEAD, wy + cosine * FOOT_IK_AHEAD, in))
		ground = under + swing * (ahead - under);
	out->delta = (ground - in->floor_z) / in->scale;
	surface_normal(wx, wy, under, sine, cosine, out->normal);
	out->ok = 1;
}

// The rotation that lays a foot on a surface, eased by `amount` and limited
static void tilt_quat(const float normal[3], float amount, float out[4]) {
	static const float UP[3] = { 0.0f, 1.0f, 0.0f };
	static const float NONE[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
	float full[4];
	float cosine;
	float angle;
	float limit = deg(FOOT_IK_MAX_TILT_DEG);

	quat_between(UP, normal, full);
	cosine = full[3] > 1.0f ? 1.0f : (full[3] < -1.0f ? -1.0f : full[3]);
	angle = 2.0f * acosf(cosine);
	if(angle > limit)
		amount *= limit / angle;
	quat_nlerp(NONE, full, amount, out);
}

static void follow_ground(character_feet* st, const foot_ground ground[2],
                          const character_feet_input* in, float dt) {
	float target = foot_ik_pelvis_target(ground[0].delta, ground[1].delta,
	                                     FOOT_IK_MAX_DROP / in->scale,
	                                     FOOT_IK_MAX_RISE / in->scale);

	if(!st->primed) {
		st->pelvis = target;
		st->raise[0] = ground[0].delta;
		st->raise[1] = ground[1].delta;
		st->primed = 1;

		return;
	}
	st->pelvis = foot_ik_chase(st->pelvis, target, FOOT_IK_PELVIS_FOLLOW, dt);
	for(int leg = 0; leg < 2; leg++)
		st->raise[leg] = foot_ik_chase(st->raise[leg], ground[leg].delta,
		                               FOOT_IK_FOOT_FOLLOW, dt);
}

float character_feet_want(int usable, int held, int airborne, float lift,
                          float distance) {
	float fade = (CHARACTER_FULL_RATE_RADIUS - distance) / FOOT_IK_FADE_BAND;

	if(!usable || held || airborne || !(lift <= FOOT_IK_AIRBORNE_LIFT) ||
	   !isfinite(distance))
		return 0.0f;

	return clamp01(fade);
}

void character_feet_update(character_feet* st, const character_legs_clip* clip,
                           const character_feet_input* in, float dt,
                           character_feet_output* out) {
	foot_ground ground[2] = { 0 };
	float want = in->want;
	int grounded = 0;

	memset(out, 0, sizeof(*out));
	out->tilt[0][3] = 1.0f;
	out->tilt[1][3] = 1.0f;
	if(!(dt > 0.0f))
		dt = 0.0f;

	if(want > 0.0f && in->scale > MIN_SCALE && isfinite(in->scale) &&
	   isfinite(in->x) && isfinite(in->y) && isfinite(in->floor_z) &&
	   isfinite(in->yaw)) {
		read_foot(clip, in, 0, &ground[0]);
		read_foot(clip, in, 1, &ground[1]);
		grounded = ground[0].ok && ground[1].ok;
	}
	if(!grounded)
		want = 0.0f;

	// A body that has just appeared has nothing to ease from; it stands as it will
	if(!st->started) {
		st->started = 1;
		st->weight = want;
	} else {
		st->weight = foot_ik_chase(st->weight, want, FOOT_IK_WEIGHT_FOLLOW, dt);
	}
	if(want <= 0.0f && st->weight < WEIGHT_OFF) {
		st->weight = 0.0f;
		st->primed = 0;

		return;
	}
	if(!(in->scale > MIN_SCALE) || !isfinite(in->scale))
		return;
	if(grounded)
		follow_ground(st, ground, in, dt);
	if(!st->primed)
		return;

	out->active = 1;
	out->lift = st->weight * st->pelvis * in->scale;
	for(int leg = 0; leg < 2; leg++) {
		out->raise[leg] = st->weight * (st->raise[leg] - st->pelvis);
		if(grounded)
			tilt_quat(ground[leg].normal, st->weight * plant(ground[leg].lift),
			          out->tilt[leg]);
	}
}
