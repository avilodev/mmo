// The leg solver and pelvis rule, on hand-made legs

#include "render/character_foot_ik.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>

static int close_to(float a, float b, float eps) {
	return fabsf(a - b) <= eps;
}

static float distance(const float a[3], const float b[3]) {
	float x = a[0] - b[0];
	float y = a[1] - b[1];
	float z = a[2] - b[2];

	return sqrtf(x * x + y * y + z * z);
}

static int finite3(const float v[3]) {
	return isfinite(v[0]) && isfinite(v[1]) && isfinite(v[2]);
}

// Hip 10 up, knee bent forward, ankle 1 up; both bones 4.61 long
static foot_ik_leg bent_leg(void) {
	foot_ik_leg leg = {
		{ 0.0f, 10.0f, 0.0f },
		{ 0.0f, 5.5f, 1.0f },
		{ 0.0f, 1.0f, 0.0f },
	};

	return leg;
}

static void test_no_raise_is_the_clip(void) {
	foot_ik_leg leg = bent_leg();
	foot_ik_result out;

	foot_ik_solve_leg(&leg, 0.0f, 0.99f, &out);
	assert(distance(out.knee, leg.knee) < 1.0e-4f);
	assert(distance(out.ankle, leg.ankle) < 1.0e-4f);
}

static void test_a_raise_bends_the_knee(void) {
	foot_ik_leg leg = bent_leg();
	foot_ik_result out;
	float l1 = distance(leg.hip, leg.knee);
	float l2 = distance(leg.knee, leg.ankle);

	foot_ik_solve_leg(&leg, 2.0f, 0.99f, &out);
	assert(close_to(out.ankle[1], 3.0f, 1.0e-4f));
	assert(close_to(out.ankle[2], 0.0f, 1.0e-4f));
	assert(out.knee[2] > leg.knee[2]);     // the knee comes further forward
	assert(close_to(out.knee[1], 6.5f, 1.0e-3f) && close_to(out.knee[2], 3.0f, 1.0e-3f));
	assert(close_to(distance(leg.hip, out.knee), l1, 1.0e-3f));
	assert(close_to(distance(out.knee, out.ankle), l2, 1.0e-3f));
}

static void test_a_small_drop_straightens_the_knee(void) {
	foot_ik_leg leg = bent_leg();
	foot_ik_result out;

	foot_ik_solve_leg(&leg, -0.1f, 0.99f, &out);
	assert(close_to(out.ankle[1], 0.9f, 1.0e-4f));
	assert(out.knee[2] < leg.knee[2]);
	assert(out.knee[2] > 0.0f);
}

static void test_out_of_reach_is_clamped(void) {
	foot_ik_leg leg = bent_leg();
	foot_ik_result out;
	float reach = (distance(leg.hip, leg.knee) + distance(leg.knee, leg.ankle)) * 0.99f;

	foot_ik_solve_leg(&leg, -3.0f, 0.99f, &out);
	assert(finite3(out.knee) && finite3(out.ankle));
	assert(close_to(distance(leg.hip, out.ankle), reach, 1.0e-3f));
}

static void test_a_big_raise_does_not_fold_flat(void) {
	foot_ik_leg leg = bent_leg();
	foot_ik_result out;

	foot_ik_solve_leg(&leg, 8.5f, 0.99f, &out);
	assert(finite3(out.knee) && finite3(out.ankle));
	assert(distance(leg.hip, out.ankle) > 0.01f);
}

static void test_a_straight_leg_bends_forward(void) {
	foot_ik_leg leg = {
		{ 0.0f, 10.0f, 0.0f },
		{ 0.0f, 5.0f, 0.0f },
		{ 0.0f, 0.0f, 0.0f },
	};
	foot_ik_result out;

	foot_ik_solve_leg(&leg, 1.0f, 0.99f, &out);
	assert(finite3(out.knee) && finite3(out.ankle));
	assert(out.knee[2] > 0.1f);
	assert(close_to(distance(leg.hip, out.knee), 5.0f, 1.0e-3f));
}

static void test_a_stretched_leg_on_flat_ground_is_left_alone(void) {
	// the clip's own leg is fully stretched (a jog's heel strike)
	foot_ik_leg leg = {
		{ 0.0f, 10.0f, 0.0f },
		{ 0.0f, 5.0f, 0.1f },
		{ 0.0f, 0.0f, 0.0f },
	};
	foot_ik_result out;

	foot_ik_solve_leg(&leg, 0.0f, 0.99f, &out);
	assert(distance(out.knee, leg.knee) < 1.0e-3f);
	assert(distance(out.ankle, leg.ankle) < 1.0e-3f);
	// asking for a little more than the clip reaches gives no more than it did
	foot_ik_solve_leg(&leg, -1.0f, 0.99f, &out);
	assert(distance(leg.hip, out.ankle) <= distance(leg.hip, leg.ankle) + 1.0e-3f);
}

static void test_bad_input_leaves_the_clip(void) {
	foot_ik_leg leg = bent_leg();
	foot_ik_result out;

	foot_ik_solve_leg(&leg, NAN, 0.99f, &out);
	assert(distance(out.knee, leg.knee) < 1.0e-6f);
	assert(distance(out.ankle, leg.ankle) < 1.0e-6f);
}

static void test_pelvis_follows_the_lower_foot(void) {
	assert(foot_ik_pelvis_target(4.0f, 0.0f, 12.0f, 12.0f) == 0.0f);
	assert(foot_ik_pelvis_target(4.0f, 2.0f, 12.0f, 12.0f) == 2.0f);
	assert(foot_ik_pelvis_target(-3.0f, 5.0f, 12.0f, 12.0f) == -3.0f);
	assert(foot_ik_pelvis_target(-50.0f, -50.0f, 12.0f, 12.0f) == -12.0f);
	assert(foot_ik_pelvis_target(50.0f, 50.0f, 12.0f, 12.0f) == 12.0f);
	assert(foot_ik_pelvis_target(NAN, 3.0f, 12.0f, 12.0f) == 3.0f);
	assert(foot_ik_pelvis_target(NAN, NAN, 12.0f, 12.0f) == 0.0f);
}

static void test_chase(void) {
	float v = 0.0f;

	assert(foot_ik_chase(0.0f, 10.0f, 18.0f, 0.0f) == 0.0f);
	assert(foot_ik_chase(0.0f, 10.0f, 18.0f, -1.0f) == 0.0f);
	for(int i = 0; i < 20; i++) {
		float next = foot_ik_chase(v, 10.0f, 18.0f, 1.0f / 60.0f);

		assert(next > v && next < 10.0f);
		v = next;
	}
	assert(close_to(foot_ik_chase(0.0f, 10.0f, 18.0f, 100.0f), 10.0f, 1.0e-4f));
	assert(foot_ik_chase(NAN, 3.0f, 18.0f, 0.1f) == 3.0f);
	assert(foot_ik_chase(2.0f, NAN, 18.0f, 0.1f) == 2.0f);
}

int main(void) {
	test_no_raise_is_the_clip();
	test_a_raise_bends_the_knee();
	test_a_small_drop_straightens_the_knee();
	test_out_of_reach_is_clamped();
	test_a_big_raise_does_not_fold_flat();
	test_a_straight_leg_bends_forward();
	test_a_stretched_leg_on_flat_ground_is_left_alone();
	test_bad_input_leaves_the_clip();
	test_pelvis_follows_the_lower_foot();
	test_chase();
	printf("character_foot_ik_test: OK\n");

	return 0;
}
