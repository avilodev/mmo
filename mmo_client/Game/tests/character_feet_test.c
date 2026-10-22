// Where the feet and the body go on stairs, ramps and flat ground

#define _POSIX_C_SOURCE 200809L
#include "footing_fixture.h"
#include "render/character_feet.h"
#include "render/character_tuning.h"
#include "world/footing.h"
#include "world_spans.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define PATH "/tmp/character_feet_test.bin"
#define PI_F 3.14159265359f

// World units per rig unit, as the renderer works it out for a 1.83 tall rig
#define SCALE (CHARACTER_HEIGHT / 1.83f)
#define REST_ANKLE 0.08f
#define STEPS 120
#define DT (1.0f / 60.0f)

static int close_to(float a, float b, float eps) {
	return fabsf(a - b) <= eps;
}

// A standing clip: leg 0 (left) a stride behind, leg 1 (right) a stride ahead
static character_legs_clip standing(float front_lift) {
	character_legs_clip clip;

	memset(&clip, 0, sizeof(clip));
	for(int leg = 0; leg < 2; leg++) {
		float z = leg == 0 ? -0.15f : 0.15f;
		float x = leg == 0 ? 0.1f : -0.1f;
		float lift = leg == 1 ? front_lift : 0.0f;

		clip.leg[leg].hip[0] = x;
		clip.leg[leg].hip[1] = 0.9f;
		clip.leg[leg].hip[2] = z;
		clip.leg[leg].knee[0] = x;
		clip.leg[leg].knee[1] = 0.5f;
		clip.leg[leg].knee[2] = z + 0.05f;
		clip.leg[leg].ankle[0] = x;
		clip.leg[leg].ankle[1] = REST_ANKLE + lift;
		clip.leg[leg].ankle[2] = z;
		clip.foot_rot[leg][3] = 1.0f;
	}

	return clip;
}

static character_feet_input body_at(float x, float floor_z, float yaw) {
	character_feet_input in;

	memset(&in, 0, sizeof(in));
	in.x = x;
	in.y = FF_Y;
	in.floor_z = floor_z;
	in.yaw = yaw;
	in.scale = SCALE;
	in.min_y = 0.0f;
	in.want = 1.0f;
	in.rest_ankle[0] = REST_ANKLE;
	in.rest_ankle[1] = REST_ANKLE;

	return in;
}

// Update a body until it has settled
static character_feet_output settle(character_feet* st, const character_legs_clip* clip,
                                    const character_feet_input* in) {
	character_feet_output out;

	for(int i = 0; i < STEPS; i++)
		character_feet_update(st, clip, in, DT, &out);

	return out;
}

// The rotated up vector's z, to read which way a tilt leans
static float tilt_lean(const float q[4]) {
	// rotating (0, 1, 0) by q: z component = 2(yz + wx) with x,y,z,w = q
	return 2.0f * (q[1] * q[2] + q[3] * q[0]);
}

static void test_flat_ground_does_nothing(void) {
	character_feet st = { 0 };
	character_legs_clip clip = standing(0.0f);
	character_feet_input in = body_at(FF_X(0, 8), 0.0f, PI_F / 2.0f);
	character_feet_output out = settle(&st, &clip, &in);

	assert(out.active);
	assert(close_to(out.lift, 0.0f, 1.0e-4f));
	assert(close_to(out.raise[0], 0.0f, 1.0e-5f));
	assert(close_to(out.raise[1], 0.0f, 1.0e-5f));
}

static void test_going_up_the_front_foot_reaches(void) {
	character_feet st = { 0 };
	character_legs_clip clip = standing(0.0f);
	// heading +x; the feet stand 3.3 units either side of cell 8 of the stairs
	character_feet_input in = body_at(FF_X(1, 8), 4.0f, PI_F / 2.0f);
	character_feet_output out = settle(&st, &clip, &in);

	// the back foot is on the 4 tread (level with the body), the front on 8
	assert(close_to(out.lift, 0.0f, 0.05f));
	assert(close_to(out.raise[0] * SCALE, 0.0f, 0.05f));
	assert(close_to(out.raise[1] * SCALE, 4.0f, 0.05f));
}

static void test_the_pelvis_follows_the_lower_foot(void) {
	character_feet st = { 0 };
	character_legs_clip clip = standing(0.0f);
	// a body standing on a floor 4 below the back tread: both feet are high
	character_feet_input in = body_at(FF_X(1, 8), 0.0f, PI_F / 2.0f);
	character_feet_output out = settle(&st, &clip, &in);

	assert(close_to(out.lift, 4.0f, 0.05f));
	assert(close_to(out.raise[0] * SCALE, 0.0f, 0.05f));
	assert(close_to(out.raise[1] * SCALE, 4.0f, 0.05f));
}

static void test_going_down_mirrors_going_up(void) {
	character_feet st = { 0 };
	character_legs_clip clip = standing(0.0f);
	// heading -x: the front (right) foot is on the lower tread
	character_feet_input in = body_at(FF_X(1, 8), 4.0f, -PI_F / 2.0f);
	character_feet_output out = settle(&st, &clip, &in);

	assert(close_to(out.lift, 0.0f, 0.05f));
	assert(close_to(out.raise[1] * SCALE, 0.0f, 0.05f));
	assert(close_to(out.raise[0] * SCALE, 4.0f, 0.05f));
}

static void test_a_swinging_foot_clears_the_riser(void) {
	character_feet st_planted = { 0 };
	character_feet st_swing = { 0 };
	character_legs_clip planted = standing(0.0f);
	character_legs_clip swing = standing(8.0f / SCALE);
	character_feet_input in = body_at(FF_X(1, 4), 0.0f, PI_F / 2.0f);
	character_feet_output a = settle(&st_planted, &planted, &in);
	character_feet_output b = settle(&st_swing, &swing, &in);

	// planted, the front foot is on the 4 tread; swinging, it looks ahead to 12
	assert(close_to(a.raise[1] * SCALE, 4.0f, 0.05f));
	assert(close_to(b.raise[1] * SCALE, 12.0f, 0.05f));
}

static void test_a_ramp_tilts_a_planted_foot(void) {
	character_feet st = { 0 };
	character_legs_clip clip = standing(0.0f);
	character_feet_input in = body_at(FF_X(2, 8), 4.0f, PI_F / 2.0f);
	character_feet_output out = settle(&st, &clip, &in);
	float lean = tilt_lean(out.tilt[1]);

	// the sole leans back atan(0.5), so up has z of minus sin of that
	assert(lean < 0.0f);
	assert(close_to(lean, -sinf(atanf(0.5f)), 0.03f));
}

static void test_a_swinging_foot_is_not_tilted(void) {
	character_feet st = { 0 };
	character_legs_clip clip = standing(10.0f / SCALE);
	character_feet_input in = body_at(FF_X(2, 8), 4.0f, PI_F / 2.0f);
	character_feet_output out = settle(&st, &clip, &in);

	assert(close_to(out.tilt[1][3], 1.0f, 1.0e-3f));
}

static void test_no_footing_wanted_means_inactive(void) {
	character_feet st = { 0 };
	character_legs_clip clip = standing(0.0f);
	character_feet_input in = body_at(FF_X(1, 8), 0.0f, PI_F / 2.0f);
	character_feet_output out;

	settle(&st, &clip, &in);
	in.want = 0.0f;
	out = settle(&st, &clip, &in);
	assert(!out.active);
	assert(out.lift == 0.0f && out.raise[0] == 0.0f && out.raise[1] == 0.0f);
	assert(st.weight == 0.0f && !st.primed);
}

// A body that has just appeared has nothing to pop from: it stands as it will
static void test_a_new_body_stands_as_it_will(void) {
	character_feet st = { 0 };
	character_feet settled = { 0 };
	character_legs_clip clip = standing(0.0f);
	character_feet_input in = body_at(FF_X(1, 8), 0.0f, PI_F / 2.0f);
	character_feet_output first;
	character_feet_output rest = settle(&settled, &clip, &in);

	character_feet_update(&st, &clip, &in, DT, &first);
	assert(first.active);
	assert(rest.lift > 0.0f);
	assert(close_to(first.lift, rest.lift, 1.0e-4f));
}

// One that was switched off and comes back eases in rather than popping
static void test_a_body_that_was_off_fades_back_in(void) {
	character_feet st = { 0 };
	character_legs_clip clip = standing(0.0f);
	character_feet_input in = body_at(FF_X(1, 8), 0.0f, PI_F / 2.0f);
	character_feet_output out = settle(&st, &clip, &in);
	float settled = out.lift;

	in.want = 0.0f;
	settle(&st, &clip, &in);
	assert(st.weight == 0.0f);
	in.want = 1.0f;
	character_feet_update(&st, &clip, &in, DT, &out);
	// 4 units wanted, only a share shown
	assert(settled > 0.0f);
	assert(out.lift > 0.0f && out.lift < settled);
}

static void test_a_teleport_stays_bounded(void) {
	character_feet st = { 0 };
	character_legs_clip clip = standing(0.0f);
	character_feet_input in = body_at(FF_X(1, 8), 4.0f, PI_F / 2.0f);
	character_feet_output out = settle(&st, &clip, &in);

	// respawn 100 higher: the ground is a drop, not something to reach
	in.floor_z = 104.0f;
	for(int i = 0; i < 5; i++) {
		character_feet_update(&st, &clip, &in, DT, &out);
		assert(isfinite(out.lift) && isfinite(out.raise[0]) && isfinite(out.raise[1]));
		assert(fabsf(out.lift) <= FOOT_IK_MAX_RISE + 0.01f);
	}
	out = settle(&st, &clip, &in);
	assert(close_to(out.lift, 0.0f, 0.05f));
	assert(close_to(out.raise[0], 0.0f, 1.0e-3f) && close_to(out.raise[1], 0.0f, 1.0e-3f));

	// and it works again when a floor is there
	in.floor_z = 4.0f;
	out = settle(&st, &clip, &in);
	assert(out.active && close_to(out.raise[1] * SCALE, 4.0f, 0.05f));
}

static void test_a_drop_is_clamped(void) {
	character_feet st = { 0 };
	character_legs_clip clip = standing(0.0f);
	// a body 10 above the ground at the flat tile: the pelvis sinks to meet it
	character_feet_input in = body_at(FF_X(0, 8), 10.0f, PI_F / 2.0f);
	character_feet_output out = settle(&st, &clip, &in);

	assert(close_to(out.lift, -10.0f, 0.05f));
	// just inside the clamp
	in.floor_z = 11.9f;
	out = settle(&st, &clip, &in);
	assert(out.lift >= -FOOT_IK_MAX_DROP - 0.01f);
	// past it the ground is a drop (a cliff edge): the body must not sink
	in.floor_z = 13.0f;
	out = settle(&st, &clip, &in);
	assert(close_to(out.lift, 0.0f, 0.05f));
}

static void test_running_down_a_flight_does_not_pop(void) {
	character_feet st = { 0 };
	character_legs_clip clip = standing(0.0f);
	// the floor lags at 10 running down; risers pass under a foot often
	character_feet_input in = body_at(FF_X(1, 12), 10.0f, -PI_F / 2.0f);
	character_feet_output out = settle(&st, &clip, &in);
	float last = out.lift;
	float last_raise[2] = { out.raise[0], out.raise[1] };

	// 180 units per second is 3 units a frame
	for(int i = 0; i < 4; i++) {
		in.x -= 3.0f;
		character_feet_update(&st, &clip, &in, DT, &out);
		// under three quarters of a riser a frame; a pop is 4 or more
		assert(fabsf(out.lift - last) < 3.0f);
		assert(fabsf(out.raise[0] - last_raise[0]) * SCALE < 3.0f);
		assert(fabsf(out.raise[1] - last_raise[1]) * SCALE < 3.0f);
		last = out.lift;
		last_raise[0] = out.raise[0];
		last_raise[1] = out.raise[1];
	}
}

static void test_who_gets_foot_ik(void) {
	float radius = CHARACTER_FULL_RATE_RADIUS;

	assert(character_feet_want(1, 0, 0, 0.0f, 10.0f) == 1.0f);
	// held in a pose, or off the ground, or without a map: none
	assert(character_feet_want(1, 1, 0, 0.0f, 10.0f) == 0.0f);
	assert(character_feet_want(1, 0, 1, 0.0f, 10.0f) == 0.0f);
	assert(character_feet_want(0, 0, 0, 0.0f, 10.0f) == 0.0f);
	// a hop of any size (an NPC's slam) is the air, not a step
	assert(character_feet_want(1, 0, 0, FOOT_IK_AIRBORNE_LIFT + 0.1f, 10.0f) == 0.0f);
	// fading out toward the full-rate radius
	assert(character_feet_want(1, 0, 0, 0.0f, radius - FOOT_IK_FADE_BAND) == 1.0f);
	assert(close_to(character_feet_want(1, 0, 0, 0.0f, radius - FOOT_IK_FADE_BAND / 2.0f),
	                0.5f, 1.0e-4f));
	assert(character_feet_want(1, 0, 0, 0.0f, radius) == 0.0f);
	assert(character_feet_want(1, 0, 0, 0.0f, radius * 3.0f) == 0.0f);
	assert(character_feet_want(1, 0, 0, NAN, 10.0f) == 0.0f);
	assert(character_feet_want(1, 0, 0, 0.0f, NAN) == 0.0f);
}

static void test_bad_input(void) {
	character_feet st = { 0 };
	character_legs_clip clip = standing(0.0f);
	character_feet_input in = body_at(FF_X(1, 8), 4.0f, PI_F / 2.0f);
	character_feet_output out;

	character_feet_update(&st, &clip, &in, 0.0f, &out);
	assert(isfinite(out.lift));
	in.scale = 0.0f;
	character_feet_update(&st, &clip, &in, DT, &out);
	assert(!out.active && out.lift == 0.0f);
	in.scale = SCALE;
	in.x = NAN;
	character_feet_update(&st, &clip, &in, DT, &out);
	assert(isfinite(out.lift) && isfinite(out.raise[0]) && isfinite(out.raise[1]));
	in.x = FF_X(1, 8);
	in.floor_z = NAN;
	character_feet_update(&st, &clip, &in, DT, &out);
	assert(isfinite(out.lift) && isfinite(out.raise[0]) && isfinite(out.raise[1]));
}

int main(void) {
	world_spans_init(CITY_ENNARA_TILE_X + 8, CITY_ENNARA_TILE_Y + 8, WORLD_TILE_PX);
	ff_write(PATH, FF_VERSION);
	assert(footing_load(PATH));

	test_flat_ground_does_nothing();
	test_going_up_the_front_foot_reaches();
	test_the_pelvis_follows_the_lower_foot();
	test_going_down_mirrors_going_up();
	test_a_swinging_foot_clears_the_riser();
	test_a_ramp_tilts_a_planted_foot();
	test_a_swinging_foot_is_not_tilted();
	test_no_footing_wanted_means_inactive();
	test_a_new_body_stands_as_it_will();
	test_a_body_that_was_off_fades_back_in();
	test_a_teleport_stays_bounded();
	test_a_drop_is_clamped();
	test_who_gets_foot_ik();
	test_running_down_a_flight_does_not_pop();
	test_bad_input();
	footing_free();
	printf("character_feet_test: OK\n");

	return 0;
}
