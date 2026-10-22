// The kit's clips on the players' models: bones matched by name, poses carried over

#include "model/skinned_model.h"
#include "render/character_pose.h"
#include "render/character_retarget.h"
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

#define DEG(x) ((x) * 3.14159265358979323846f / 180.0f)

static SkinnedModel g_kit, g_male, g_female;
static character_retarget g_retarget;
static Pose g_kit_pose, g_out;
static SkinnedInstance g_instance;

// The angle between two rotations, radians; exact for rotations that are close
static float quat_angle(const float a[4], const float b[4]) {
	float dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
	float sign = dot < 0.0f ? -1.0f : 1.0f;
	float sum = 0.0f;

	for(int i = 0; i < 4; i++)
		sum += (a[i] - sign * b[i]) * (a[i] - sign * b[i]);

	return 2.0f * asinf(fminf(0.5f * sqrtf(sum), 1.0f));
}

// Rotate a node of the kit's pose by `radians` about its own y axis, from its rest
static void roll_kit_node(int node, float radians) {
	static const float AXIS_Y[3] = { 0.0f, 1.0f, 0.0f };
	float turn[4];
	float result[4];

	quat_from_axis_angle(AXIS_Y, radians, turn);
	quat_multiply(g_kit.skeleton.bind_local[node].rotation, turn, result);
	memcpy(g_kit_pose.local[node].rotation, result, sizeof(result));
}

// The roll of `node` in g_out away from the target's rest, radians
static float rolled(const SkinnedModel* target, int node) {
	const float* rest = target->skeleton.bind_local[node].rotation;
	const float inverse[4] = { -rest[0], -rest[1], -rest[2], rest[3] };
	float delta[4];

	quat_multiply(inverse, g_out.local[node].rotation, delta);
	return character_retarget_roll(delta);
}

static void check_rest(const SkinnedModel* target) {
	float worst = 0.0f;
	float moved = 0.0f;

	skinned_model_bind_pose(&g_kit, &g_kit_pose);
	character_retarget_apply(&g_retarget, &g_kit, target, &g_kit_pose, &g_out);

	for(int i = 0; i < target->skeleton.node_count; i++) {
		float angle = quat_angle(g_out.local[i].rotation, target->skeleton.bind_local[i].rotation);

		if(angle > worst)
			worst = angle;

		for(int c = 0; c < 3; c++) {
			float d = fabsf(g_out.local[i].translation[c] - target->skeleton.bind_local[i].translation[c]);

			if(d > moved)
				moved = d;
		}
	}

	CHECK(worst < 1e-3f, "the kit at rest puts the model at rest");
	CHECK(moved < 1e-6f, "the kit at rest moves no bone");
}

static void check_carry(const SkinnedModel* target) {
	int spine = character_pose_find_node(&g_kit, "spine_01");
	int spine_target = character_pose_find_node(target, "spine_01");
	int pelvis = character_pose_find_node(&g_kit, "pelvis");
	int pelvis_target = character_pose_find_node(target, "pelvis");
	int loose = character_pose_find_node(target, "J_Sec_L_Bust1");
	float turn[4];
	static const float AXIS_X[3] = { 1.0f, 0.0f, 0.0f };

	skinned_model_bind_pose(&g_kit, &g_kit_pose);
	quat_from_axis_angle(AXIS_X, DEG(25.0f), turn);
	memcpy(g_kit_pose.local[spine].rotation, turn, sizeof(turn));
	g_kit_pose.local[pelvis].translation[2] += 0.10f;
	character_retarget_apply(&g_retarget, &g_kit, target, &g_kit_pose, &g_out);

	CHECK(quat_angle(g_out.local[spine_target].rotation, turn) < 1e-4f,
	      "a bone of the same name takes the kit's rotation");

	float lift = g_out.local[pelvis_target].translation[2] -
	             target->skeleton.bind_local[pelvis_target].translation[2];

	CHECK(fabsf(lift - 0.10f * g_retarget.move_scale) < 1e-5f,
	      "the pelvis rises as far as the kit's, times the height ratio");
	CHECK(g_retarget.move_scale > 0.8f && g_retarget.move_scale < 1.3f,
	      "the height ratio is near one");

	if(loose >= 0)
		CHECK(quat_angle(g_out.local[loose].rotation, target->skeleton.bind_local[loose].rotation) < 1e-6f,
		      "a bone the kit lacks stays as it was made");
}

static void check_twist(const SkinnedModel* target) {
	int fore = character_pose_find_node(&g_kit, "lowerarm_l");
	int first = character_pose_find_node(target, "lowerarm_twist_01_l");
	int second = character_pose_find_node(target, "lowerarm_twist_02_l");

	skinned_model_bind_pose(&g_kit, &g_kit_pose);
	roll_kit_node(fore, DEG(90.0f));
	character_retarget_apply(&g_retarget, &g_kit, target, &g_kit_pose, &g_out);
	CHECK(fabsf(rolled(target, first) - DEG(-60.0f) * CHARACTER_TWIST_01_COUNTER) < 1e-3f,
	      "a forearm rolled 90 degrees: the near twist bone takes back 60");
	CHECK(fabsf(rolled(target, second) - DEG(-60.0f) * CHARACTER_TWIST_02_COUNTER) < 1e-3f,
	      "a forearm rolled 90 degrees: the far twist bone takes back 30");

	roll_kit_node(fore, DEG(-90.0f));
	character_retarget_apply(&g_retarget, &g_kit, target, &g_kit_pose, &g_out);
	CHECK(fabsf(rolled(target, first) - DEG(60.0f) * CHARACTER_TWIST_01_COUNTER) < 1e-3f,
	      "rolled the other way, it turns the other way");

	roll_kit_node(fore, DEG(20.0f));
	character_retarget_apply(&g_retarget, &g_kit, target, &g_kit_pose, &g_out);
	CHECK(fabsf(rolled(target, first)) < 1e-4f, "a roll inside the dead zone leaves them alone");
}

// Skin a walking frame: every vertex finite and the body about as tall as made
static void check_walk(const SkinnedModel* target, float low, float high) {
	int clip = skinned_model_find_clip(&g_kit, CHARACTER_CLIP_WALK);
	float lowest = 1e9f;
	float highest = -1e9f;
	int finite = 1;

	skinned_model_sample(&g_kit, clip, 0.3f, &g_kit_pose);
	character_retarget_apply(&g_retarget, &g_kit, target, &g_kit_pose, &g_out);
	skinned_instance_init(&g_instance, target);
	skinned_model_skin(target, &g_out, &g_instance);

	for(int m = 0; m < g_instance.mesh_count; m++) {
		const SkinnedMeshVertices* verts = &g_instance.meshes[m];

		for(int v = 0; v < verts->vertex_count; v++) {
			float y = verts->vertices[v * SKIN_VERTEX_FLOATS + 1];

			if(!isfinite(y))
				finite = 0;
			if(y < lowest)
				lowest = y;
			if(y > highest)
				highest = y;
		}
	}

	skinned_instance_free(&g_instance);
	CHECK(finite, "a walking frame skins to finite vertices");
	CHECK(lowest > -0.15f && highest > low && highest < high,
	      "a walking frame stands on the floor at about the height it was made");
}

static void check_player(const char* label, SkinnedModel* target, const char* path,
                         float low, float high) {
	char error[256] = "";

	printf("%s\n", label);
	if(!skinned_model_load_glb(target, path, error, (int)sizeof(error))) {
		printf("  FAIL could not load %s: %s\n", path, error);
		g_failures++;
		return;
	}

	CHECK(target->skeleton.joint_count > 128, "it loads with more joints than the old limit");
	CHECK(character_retarget_build(&g_kit, target, &g_retarget) >= 50,
	      "most of the kit's bones are matched by name");
	CHECK(g_retarget.twist_count == 4, "both forearms have both twist bones");
	CHECK(g_retarget.moved_count == 2, "the root and the pelvis follow the kit's translation");

	check_rest(target);
	check_carry(target);
	check_twist(target);
	check_walk(target, low, high);
	skinned_model_free(target);
}

int main(void) {
	char error[256] = "";

	printf("character_retarget_test\n");
	if(!skinned_model_load_glb(&g_kit, CHARACTER_MODEL_PATH, error, (int)sizeof(error))) {
		printf("  FAIL could not load %s: %s\n", CHARACTER_MODEL_PATH, error);
		return 1;
	}

	check_player("male", &g_male, CHARACTER_MODEL_MALE_PATH, 1.6f, 2.0f);
	check_player("female", &g_female, CHARACTER_MODEL_FEMALE_PATH, 1.4f, 1.9f);

	{
		float none[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
		float quarter[4] = { 0.0f, sinf(DEG(45.0f)), 0.0f, cosf(DEG(45.0f)) };
		float flipped[4] = { 0.0f, -sinf(DEG(45.0f)), 0.0f, -cosf(DEG(45.0f)) };

		printf("roll\n");
		CHECK(fabsf(character_retarget_roll(none)) < 1e-6f, "no turn is no roll");
		CHECK(fabsf(character_retarget_roll(quarter) - DEG(90.0f)) < 1e-5f, "a quarter turn about y");
		CHECK(fabsf(character_retarget_roll(flipped) - DEG(90.0f)) < 1e-5f,
		      "the same rotation with its signs flipped reads the same");
	}

	skinned_model_free(&g_kit);
	printf(g_failures ? "%d FAILED\n" : "all passed\n", g_failures);
	return g_failures ? 1 : 0;
}
