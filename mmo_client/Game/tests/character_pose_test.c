// Procedural offsets move the shipped rig the right way; masks spare the legs

#include "model/skinned_model.h"
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
static Pose g_idle, g_moved, g_sprint, g_out;

static int node_named(const char* name) {
	for(int i = 0; i < g_model.skeleton.node_count; i++)
		if(strcmp(g_model.skeleton.node_names[i], name) == 0)
			return i;

	return -1;
}

// Where a bone's joint sits in the model's frame
static void node_position(const Pose* pose, int node, float out[3]) {
	const Skeleton* skeleton = &g_model.skeleton;
	Mat4 global[SKIN_MAX_NODES];
	const float origin[3] = { 0.0f, 0.0f, 0.0f };

	for(int i = 0; i < skeleton->node_count; i++) {
		int n = skeleton->order[i];
		const Transform* local = &pose->local[n];
		Mat4 matrix = mat4_from_trs(local->translation, local->rotation, local->scale);
		int parent = skeleton->parent[n];

		global[n] = parent < 0 ? matrix : mat4_multiply(global[parent], matrix);
	}
	mat4_transform_point(&global[node], origin, out);
}

// A bone's orientation in the model's frame, as a quaternion
static void node_rotation(const Pose* pose, int node, float out[4]) {
	const Skeleton* skeleton = &g_model.skeleton;
	int chain[SKIN_MAX_NODES];
	int count = 0;
	float next[4];

	out[0] = out[1] = out[2] = 0.0f;
	out[3] = 1.0f;
	for(int n = node; n >= 0; n = skeleton->parent[n])
		chain[count++] = n;
	for(int i = count - 1; i >= 0; i--) {
		quat_multiply(out, pose->local[chain[i]].rotation, next);
		memcpy(out, next, sizeof(next));
	}
}

// How far a bone turned between two poses, as the axis and angle of the change
static void node_turn(const Pose* before, const Pose* after, int node, float axis[3],
                      float* angle) {
	float a[4], b[4], inverse[4], delta[4];
	float s;

	node_rotation(before, node, a);
	node_rotation(after, node, b);
	inverse[0] = -a[0];
	inverse[1] = -a[1];
	inverse[2] = -a[2];
	inverse[3] = a[3];
	quat_multiply(b, inverse, delta);
	if(delta[3] < 0.0f)
		for(int i = 0; i < 4; i++)
			delta[i] = -delta[i];
	s = sqrtf(delta[0] * delta[0] + delta[1] * delta[1] + delta[2] * delta[2]);
	*angle = 2.0f * atan2f(s, delta[3]);
	for(int i = 0; i < 3; i++)
		axis[i] = s > 1e-6f ? delta[i] / s : 0.0f;
}

static int pose_equal(const Pose* a, const Pose* b) {
	return memcmp(a->local, b->local,
	              sizeof(Transform) * (size_t)g_model.skeleton.node_count) == 0;
}

static float rotation_diff(const Pose* a, const Pose* b, int node) {
	float d = 0.0f;

	for(int i = 0; i < 4; i++)
		d += fabsf(a->local[node].rotation[i] - b->local[node].rotation[i]);

	return d;
}

int main(void) {
	char error[256] = "";
	character_rig rig;
	character_offsets none = { 0 };
	float mask[SKIN_MAX_NODES];

	printf("=== character pose ===\n");

	if(!skinned_model_load_glb(&g_model, CHARACTER_MODEL_PATH, error,
	                           (int)sizeof(error))) {
		printf("  FAIL could not load %s: %s\n", CHARACTER_MODEL_PATH, error);
		return 1;
	}
	skinned_model_sample(&g_model, skinned_model_find_clip(&g_model, CHARACTER_CLIP_IDLE),
	                     0.0f, &g_idle);

	printf("\nTEST 1: every bone the layer moves is on the rig\n");
	CHECK(character_rig_find(&g_model, &rig), "spine, neck and head are found");

	printf("\nTEST 2: the rig faces +z with +x on its left\n");
	{
		float thigh_l[3], thigh_r[3], foot_l[3], ball_l[3];

		node_position(&g_idle, node_named("thigh_l"), thigh_l);
		node_position(&g_idle, node_named("thigh_r"), thigh_r);
		node_position(&g_idle, node_named("foot_l"), foot_l);
		node_position(&g_idle, node_named("ball_l"), ball_l);
		CHECK(thigh_l[0] > thigh_r[0], "the left thigh is on +x");
		CHECK(ball_l[2] > foot_l[2], "the toes point +z");
	}

	printf("\nTEST 3: no offsets leave the pose untouched\n");
	g_moved = g_idle;
	character_pose_apply(&g_model, &rig, &none, &g_moved);
	CHECK(pose_equal(&g_idle, &g_moved), "a zero offset changes nothing");

	printf("\nTEST 4: leaning forward moves the head forward and the hips not at all\n");
	{
		character_offsets lean = { 0.2f, 0.0f, 0.0f, 0.0f };
		float before[3], after[3], hips_before[3], hips_after[3];
		int head = node_named("Head"), pelvis = node_named("pelvis");

		g_moved = g_idle;
		character_pose_apply(&g_model, &rig, &lean, &g_moved);
		node_position(&g_idle, head, before);
		node_position(&g_moved, head, after);
		node_position(&g_idle, pelvis, hips_before);
		node_position(&g_moved, pelvis, hips_after);
		CHECK(after[2] > before[2] + 0.05f, "the head moves toward +z");
		CHECK(fabsf(after[0] - before[0]) < 0.01f, "and not sideways");
		CHECK(after[1] < before[1], "and drops a little");
		CHECK(fabsf(hips_after[2] - hips_before[2]) < 1e-5f, "the hips stay put");
	}

	printf("\nTEST 5: leaning left moves the head to +x\n");
	{
		character_offsets lean = { 0.0f, 0.2f, 0.0f, 0.0f };
		float before[3], after[3];
		int head = node_named("Head");

		g_moved = g_idle;
		character_pose_apply(&g_model, &rig, &lean, &g_moved);
		node_position(&g_idle, head, before);
		node_position(&g_moved, head, after);
		CHECK(after[0] > before[0] + 0.05f, "the head moves toward +x");
		CHECK(fabsf(after[2] - before[2]) < 0.01f, "and not forward");
	}

	printf("\nTEST 6: the twist turns the upper body about the up axis\n");
	{
		character_offsets twist = { 0.0f, 0.0f, 0.5f, 0.0f };
		float axis[3], angle;

		g_moved = g_idle;
		character_pose_apply(&g_model, &rig, &twist, &g_moved);
		node_turn(&g_idle, &g_moved, node_named("spine_03"), axis, &angle);
		CHECK(fabsf(angle - 0.5f) < 0.01f, "spine_03 turns by the whole twist");
		CHECK(axis[1] > 0.99f, "about +y, toward increasing yaw");
		node_turn(&g_idle, &g_moved, node_named("spine_01"), axis, &angle);
		CHECK(fabsf(angle - 0.5f * CHARACTER_SPINE_SHARE_1) < 0.01f,
		      "spine_01 takes only its share");
		node_turn(&g_idle, &g_moved, node_named("pelvis"), axis, &angle);
		CHECK(angle < 1e-4f, "the hips do not turn");
	}

	printf("\nTEST 7: the head's yaw adds on top of the spine's\n");
	{
		character_offsets look = { 0.0f, 0.0f, 0.3f, 0.4f };
		float axis[3], angle;

		g_moved = g_idle;
		character_pose_apply(&g_model, &rig, &look, &g_moved);
		node_turn(&g_idle, &g_moved, node_named("Head"), axis, &angle);
		CHECK(fabsf(angle - 0.7f) < 0.01f, "the head turns by spine plus its own yaw");
		CHECK(axis[1] > 0.99f, "about +y");
	}

	printf("\nTEST 8: the largest offsets still skin to a sane body\n");
	{
		character_offsets worst = { 0.25f, 0.21f, 0.7f, 1.13f };
		SkinnedInstance body;
		float reach = 0.0f;
		int bad = 0;

		g_moved = g_idle;
		character_pose_apply(&g_model, &rig, &worst, &g_moved);
		skinned_instance_init(&body, &g_model);
		skinned_model_skin(&g_model, &g_moved, &body);

		for(int m = 0; m < body.mesh_count; m++) {
			for(int i = 0; i < body.meshes[m].vertex_count; i++) {
				const float* v = body.meshes[m].vertices + (size_t)i * SKIN_VERTEX_FLOATS;
				float d = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);

				if(!isfinite(d))
					bad = 1;
				else if(d > reach)
					reach = d;
			}
		}

		CHECK(!bad && reach < 3.0f, "every vertex stays near the body");
		skinned_instance_free(&body);
	}

	printf("\nTEST 9: the upper-body mask covers the arms and head, not the legs\n");
	CHECK(skinned_model_mask_subtree(&g_model, "spine_01", mask), "spine_01 exists");
	CHECK(mask[node_named("spine_01")] == 1.0f, "the root bone is in");
	CHECK(mask[node_named("upperarm_l")] == 1.0f, "the arms are in");
	CHECK(mask[node_named("Head")] == 1.0f, "the head is in");
	CHECK(mask[node_named("pelvis")] == 0.0f, "the hips are out");
	CHECK(mask[node_named("thigh_l")] == 0.0f, "the legs are out");
	CHECK(!skinned_model_mask_subtree(&g_model, "no_such_bone", mask), "a bad name fails");
	{
		int any = 0;

		for(int i = 0; i < SKIN_MAX_NODES; i++)
			any |= mask[i] != 0.0f;
		CHECK(!any, "and leaves the mask empty");
	}

	printf("\nTEST 10: a masked blend takes the clip on the masked bones only\n");
	{
		int sprint = skinned_model_find_clip(&g_model, CHARACTER_CLIP_RUN);
		int thigh = node_named("thigh_l"), arm = node_named("upperarm_l");

		skinned_model_sample(&g_model, sprint, 0.3f, &g_sprint);
		skinned_model_mask_subtree(&g_model, "spine_01", mask);
		skinned_model_blend_masked(&g_model, &g_idle, &g_sprint, 1.0f, mask, &g_out);
		CHECK(rotation_diff(&g_out, &g_idle, thigh) < 1e-6f, "the legs stay as in a");
		CHECK(rotation_diff(&g_out, &g_sprint, arm) < 1e-5f, "the arms are fully b");
		CHECK(rotation_diff(&g_idle, &g_sprint, thigh) > 0.05f,
		      "(the clips do differ at the legs)");

		skinned_model_blend_masked(&g_model, &g_idle, &g_sprint, 0.0f, mask, &g_out);
		CHECK(pose_equal(&g_out, &g_idle), "t = 0 is exactly a");

		skinned_model_blend_masked(&g_model, &g_idle, &g_sprint, 0.5f, mask, &g_out);
		CHECK(rotation_diff(&g_out, &g_idle, arm) > 0.01f &&
		      rotation_diff(&g_out, &g_sprint, arm) > 0.01f,
		      "t = 0.5 lands between a and b");
	}

	skinned_model_free(&g_model);
	printf("\n%s\n", g_failures ? "FAILED" : "all passed");

	return g_failures ? 1 : 0;
}
