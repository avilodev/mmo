// The legs of the shipped rig: found, measured, and bent to a solution

#include "model/skinned_model.h"
#include "render/character_foot_ik.h"
#include "render/character_legs.h"
#include "render/character_pose.h"
#include "render/character_tuning.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define STB_IMAGE_IMPLEMENTATION
#include "utils/stb_image.h"

static int g_failures = 0;

#define CHECK(cond, what)                                                   \
	do {                                                                    \
		if(cond) {                                                          \
			printf("  ok   %s\n", (what));                                  \
		} else {                                                            \
			printf("  FAIL %s  (%s:%d)\n", (what), __FILE__, __LINE__);     \
			g_failures++;                                                   \
		}                                                                   \
	} while(0)

static SkinnedModel g_model;
static Pose g_walk, g_bent;
static character_legs_rig g_rig;

static float distance(const float a[3], const float b[3]) {
	float x = a[0] - b[0];
	float y = a[1] - b[1];
	float z = a[2] - b[2];

	return sqrtf(x * x + y * y + z * z);
}

// The angle between two rotations, radians
static float quat_angle(const float a[4], const float b[4]) {
	float dot = fabsf(a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]);

	return 2.0f * acosf(dot > 1.0f ? 1.0f : dot);
}

static int is_leg_node(int node) {
	for(int leg = 0; leg < 2; leg++)
		if(node == g_rig.thigh[leg] || node == g_rig.calf[leg] || node == g_rig.foot[leg])
			return 1;

	return 0;
}

int main(void) {
	char error[256] = "";
	character_legs_clip clip, after;
	foot_ik_result result[2];
	static const float IDENTITY[2][4] = { { 0, 0, 0, 1 }, { 0, 0, 0, 1 } };
	float height;
	float raise;

	printf("=== character legs ===\n");
	if(!skinned_model_load_glb(&g_model, CHARACTER_MODEL_PATH, error, (int)sizeof(error))) {
		printf("  FAIL could not load %s: %s\n", CHARACTER_MODEL_PATH, error);
		return 1;
	}
	height = g_model.max_y - g_model.min_y;
	raise = 0.03f * height;
	skinned_model_sample(&g_model, skinned_model_find_clip(&g_model, CHARACTER_CLIP_WALK),
	                     0.2f, &g_walk);

	printf("\nTEST 1: the leg bones are on the rig\n");
	CHECK(character_legs_rig_find(&g_model, &g_rig), "thigh, calf and foot are found");
	CHECK(g_rig.rest_ankle[0] > 0.0f && g_rig.rest_ankle[0] < 0.1f * height,
	      "the left ankle rests a little above the floor");
	CHECK(fabsf(g_rig.rest_ankle[0] - g_rig.rest_ankle[1]) < 0.01f * height,
	      "both ankles rest at the same height");

	printf("\nTEST 2: the legs are read off a pose\n");
	character_legs_measure(&g_model, &g_rig, &g_walk, &clip);
	CHECK(clip.leg[0].hip[0] > clip.leg[1].hip[0], "the left hip is on +x");
	CHECK(clip.leg[0].knee[1] < clip.leg[0].hip[1], "the knee is below the hip");
	CHECK(clip.leg[0].ankle[1] < clip.leg[0].knee[1], "the ankle is below the knee");
	CHECK(distance(clip.leg[1].hip, clip.leg[1].knee) > 0.1f * height,
	      "the thigh has length");

	printf("\nTEST 3: a solution that is the clip changes nothing\n");
	for(int leg = 0; leg < 2; leg++) {
		memcpy(result[leg].knee, clip.leg[leg].knee, sizeof(result[leg].knee));
		memcpy(result[leg].ankle, clip.leg[leg].ankle, sizeof(result[leg].ankle));
	}
	g_bent = g_walk;
	character_legs_apply(&g_model, &g_rig, &clip, result, IDENTITY, &g_bent);
	{
		float worst = 0.0f;

		for(int n = 0; n < g_model.skeleton.node_count; n++)
			for(int i = 0; i < 4; i++)
				worst = fmaxf(worst, fabsf(g_walk.local[n].rotation[i] -
				                           g_bent.local[n].rotation[i]));
		CHECK(worst < 1.0e-4f, "every bone is where the clip had it");
	}

	printf("\nTEST 4: a raised target is reached, with the bones' lengths\n");
	for(int leg = 0; leg < 2; leg++)
		foot_ik_solve_leg(&clip.leg[leg], raise, 0.99f, &result[leg]);
	g_bent = g_walk;
	character_legs_apply(&g_model, &g_rig, &clip, result, IDENTITY, &g_bent);
	character_legs_measure(&g_model, &g_rig, &g_bent, &after);
	for(int leg = 0; leg < 2; leg++) {
		CHECK(distance(after.leg[leg].ankle, result[leg].ankle) < 0.002f * height,
		      "the ankle lands on its target");
		CHECK(distance(after.leg[leg].knee, result[leg].knee) < 0.002f * height,
		      "the knee lands where it was solved");
		CHECK(fabsf(distance(after.leg[leg].hip, after.leg[leg].knee) -
		            distance(clip.leg[leg].hip, clip.leg[leg].knee)) < 0.001f * height,
		      "the thigh keeps its length");
		CHECK(distance(after.leg[leg].hip, clip.leg[leg].hip) < 1.0e-4f * height,
		      "the hip has not moved");
	}

	printf("\nTEST 5: a foot tilt turns the foot, not the bones around it\n");
	// leg 1 is still bent by TEST 4; only its foot orientation is checked
	{
		float tilt[2][4] = { { 0, 0, 0, 1 }, { 0, 0, 0, 1 } };
		float expected[4];
		float half = 0.5f * 20.0f * 3.14159265f / 180.0f;

		tilt[0][0] = sinf(half);           // 20 degrees about +x
		tilt[0][3] = cosf(half);
		memcpy(result[0].knee, clip.leg[0].knee, sizeof(result[0].knee));
		memcpy(result[0].ankle, clip.leg[0].ankle, sizeof(result[0].ankle));
		g_bent = g_walk;
		character_legs_apply(&g_model, &g_rig, &clip, result, tilt, &g_bent);
		character_legs_measure(&g_model, &g_rig, &g_bent, &after);
		quat_multiply(tilt[0], clip.foot_rot[0], expected);
		CHECK(quat_angle(after.foot_rot[0], expected) < 0.002f,
		      "the foot's world orientation is the tilt times the clip's");
		CHECK(quat_angle(after.foot_rot[1], clip.foot_rot[1]) < 0.002f,
		      "the other foot keeps its world orientation");
		CHECK(distance(after.leg[0].ankle, clip.leg[0].ankle) < 1.0e-4f * height,
		      "the ankle stays put");
	}

	printf("\nTEST 6: no other bone is touched\n");
	{
		int touched = 0;

		for(int n = 0; n < g_model.skeleton.node_count; n++)
			if(!is_leg_node(n) &&
			   memcmp(&g_walk.local[n], &g_bent.local[n], sizeof(Transform)) != 0)
				touched++;
		CHECK(touched == 0, "only thighs, calves and feet change");
	}

	if(g_failures) {
		printf("\n%d check(s) failed\n", g_failures);
		return 1;
	}
	printf("\nall checks passed\n");

	return 0;
}
