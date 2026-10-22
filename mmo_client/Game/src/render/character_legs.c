// Find the rig's legs, read them off a pose, and bend them to a solution.

#include "render/character_legs.h"
#include "render/character_pose.h"
#include "render/character_tuning.h"

#include <math.h>
#include <string.h>

// Scratch for the measurements; rendering is single-threaded
static Mat4 g_global[SKIN_MAX_NODES];
static Pose g_scratch;

static void pose_globals(const SkinnedModel* model, const Pose* pose) {
	const Skeleton* skeleton = &model->skeleton;

	for(int i = 0; i < skeleton->node_count; i++) {
		int n = skeleton->order[i];
		const Transform* local = &pose->local[n];
		Mat4 matrix = mat4_from_trs(local->translation, local->rotation, local->scale);
		int parent = skeleton->parent[n];

		g_global[n] = parent < 0 ? matrix : mat4_multiply(g_global[parent], matrix);
	}
}

static void joint_position(int node, float out[3]) {
	const float origin[3] = { 0.0f, 0.0f, 0.0f };

	mat4_transform_point(&g_global[node], origin, out);
}

void character_legs_measure(const SkinnedModel* model, const character_legs_rig* rig,
                            const Pose* pose, character_legs_clip* out) {
	pose_globals(model, pose);
	for(int leg = 0; leg < 2; leg++) {
		float parent[4];

		joint_position(rig->thigh[leg], out->leg[leg].hip);
		joint_position(rig->calf[leg], out->leg[leg].knee);
		joint_position(rig->foot[leg], out->leg[leg].ankle);
		character_pose_parent_rotation(model, pose, rig->foot[leg], parent);
		quat_multiply(parent, pose->local[rig->foot[leg]].rotation, out->foot_rot[leg]);
	}
}

int character_legs_rig_find(const SkinnedModel* model, character_legs_rig* rig) {
	static const char* const THIGH[2] = { CHARACTER_BONE_THIGH_L, CHARACTER_BONE_THIGH_R };
	static const char* const CALF[2] = { CHARACTER_BONE_CALF_L, CHARACTER_BONE_CALF_R };
	static const char* const FOOT[2] = { CHARACTER_BONE_FOOT_L, CHARACTER_BONE_FOOT_R };
	character_legs_clip bind;
	int ok = 1;

	for(int leg = 0; leg < 2; leg++) {
		rig->thigh[leg] = character_pose_find_node(model, THIGH[leg]);
		rig->calf[leg] = character_pose_find_node(model, CALF[leg]);
		rig->foot[leg] = character_pose_find_node(model, FOOT[leg]);
		rig->rest_ankle[leg] = 0.0f;
		ok = ok && rig->thigh[leg] >= 0 && rig->calf[leg] >= 0 && rig->foot[leg] >= 0;
	}
	if(!ok)
		return 0;

	skinned_model_bind_pose(model, &g_scratch);
	character_legs_measure(model, rig, &g_scratch, &bind);
	for(int leg = 0; leg < 2; leg++)
		rig->rest_ankle[leg] = bind.leg[leg].ankle[1] - model->min_y;

	return 1;
}

// The unit direction from a to b
static void direction(const float a[3], const float b[3], float out[3]) {
	float length;

	for(int i = 0; i < 3; i++)
		out[i] = b[i] - a[i];
	length = sqrtf(out[0] * out[0] + out[1] * out[1] + out[2] * out[2]);
	if(length < 1.0e-8f) {
		out[0] = 0.0f;
		out[1] = -1.0f;
		out[2] = 0.0f;

		return;
	}
	for(int i = 0; i < 3; i++)
		out[i] /= length;
}

static void rotate_vector(const float q[4], const float v[3], float out[3]) {
	float p[4] = { v[0], v[1], v[2], 0.0f };
	float inverse[4] = { -q[0], -q[1], -q[2], q[3] };
	float step[4];
	float turned[4];

	quat_multiply(q, p, step);
	quat_multiply(step, inverse, turned);
	out[0] = turned[0];
	out[1] = turned[1];
	out[2] = turned[2];
}

// Set a foot's local rotation so its world orientation is `want`
static void orient_foot(const SkinnedModel* model, Pose* pose, int node,
                        const float want[4]) {
	float parent[4];
	float inverse[4];
	float local[4];

	character_pose_parent_rotation(model, pose, node, parent);
	inverse[0] = -parent[0];
	inverse[1] = -parent[1];
	inverse[2] = -parent[2];
	inverse[3] = parent[3];
	quat_multiply(inverse, want, local);
	memcpy(pose->local[node].rotation, local, sizeof(local));
}

void character_legs_apply(const SkinnedModel* model, const character_legs_rig* rig,
                          const character_legs_clip* clip,
                          const foot_ik_result result[2], const float tilt[2][4],
                          Pose* pose) {
	for(int leg = 0; leg < 2; leg++) {
		const foot_ik_leg* c = &clip->leg[leg];
		float old_thigh[3];
		float new_thigh[3];
		float old_shin[3];
		float new_shin[3];
		float turn[4];
		float want[4];

		// the thigh swings the knee onto its solved place
		direction(c->hip, c->knee, old_thigh);
		direction(c->hip, result[leg].knee, new_thigh);
		quat_between(old_thigh, new_thigh, turn);
		character_pose_turn_node(model, pose, rig->thigh[leg], turn);

		// the calf, carried by the thigh, swings the ankle onto its place
		direction(c->knee, c->ankle, old_shin);
		rotate_vector(turn, old_shin, old_shin);
		direction(result[leg].knee, result[leg].ankle, new_shin);
		quat_between(old_shin, new_shin, turn);
		character_pose_turn_node(model, pose, rig->calf[leg], turn);

		// the foot keeps the world orientation the clip gave it, plus the tilt
		quat_multiply(tilt[leg], clip->foot_rot[leg], want);
		orient_foot(model, pose, rig->foot[leg], want);
	}
}
