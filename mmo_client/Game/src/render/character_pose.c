// Move the rig's spine, neck and head by the procedural offsets.

#include "render/character_pose.h"
#include "render/character_tuning.h"

#include <math.h>
#include <string.h>

// Offsets smaller than this (radians) are not worth a rotation
#define TINY_ANGLE 1e-5f

int character_pose_find_node(const SkinnedModel* model, const char* name) {
	const Skeleton* skeleton = &model->skeleton;

	for(int i = 0; i < skeleton->node_count; i++)
		if(strcmp(skeleton->node_names[i], name) == 0)
			return i;

	return -1;
}

int character_rig_find(const SkinnedModel* model, character_rig* rig) {
	rig->spine[0] = character_pose_find_node(model, CHARACTER_BONE_SPINE_1);
	rig->spine[1] = character_pose_find_node(model, CHARACTER_BONE_SPINE_2);
	rig->spine[2] = character_pose_find_node(model, CHARACTER_BONE_SPINE_3);
	rig->neck = character_pose_find_node(model, CHARACTER_BONE_NECK);
	rig->head = character_pose_find_node(model, CHARACTER_BONE_HEAD);

	return rig->spine[0] >= 0 && rig->spine[1] >= 0 && rig->spine[2] >= 0 &&
	       rig->neck >= 0 && rig->head >= 0;
}

void character_pose_parent_rotation(const SkinnedModel* model, const Pose* pose,
                                    int node, float out[4]) {
	const Skeleton* skeleton = &model->skeleton;
	int chain[SKIN_MAX_NODES];
	int count = 0;
	float next[4];

	out[0] = out[1] = out[2] = 0.0f;
	out[3] = 1.0f;
	if(node < 0 || node >= skeleton->node_count)
		return;
	for(int p = skeleton->parent[node]; p >= 0 && count < SKIN_MAX_NODES;
	    p = skeleton->parent[p])
		chain[count++] = p;
	for(int i = count - 1; i >= 0; i--) {
		quat_multiply(out, pose->local[chain[i]].rotation, next);
		memcpy(out, next, sizeof(next));
	}
}

void character_pose_turn_node(const SkinnedModel* model, Pose* pose, int node,
                              const float q[4]) {
	float parent[4];
	float parent_inverse[4];
	float step[4];
	float world[4];
	float local[4];

	if(node < 0 || node >= model->skeleton.node_count)
		return;

	// local' = parent^-1 * turn * parent * local
	character_pose_parent_rotation(model, pose, node, parent);
	parent_inverse[0] = -parent[0];
	parent_inverse[1] = -parent[1];
	parent_inverse[2] = -parent[2];
	parent_inverse[3] = parent[3];
	quat_multiply(q, parent, step);
	quat_multiply(parent_inverse, step, world);
	quat_multiply(world, pose->local[node].rotation, local);
	memcpy(pose->local[node].rotation, local, sizeof(local));
}

// Turn a bone by `angle` about a model-frame axis through its own joint
static void rotate_bone(const SkinnedModel* model, Pose* pose, int node,
                        const float axis[3], float angle) {
	float turn[4];

	if(node < 0 || node >= model->skeleton.node_count || fabsf(angle) < TINY_ANGLE)
		return;
	quat_from_axis_angle(axis, angle, turn);
	character_pose_turn_node(model, pose, node, turn);
}

void character_pose_apply(const SkinnedModel* model, const character_rig* rig,
                          const character_offsets* offsets, Pose* pose) {
	static const float AXIS_X[3] = { 1.0f, 0.0f, 0.0f };
	static const float AXIS_Y[3] = { 0.0f, 1.0f, 0.0f };
	static const float AXIS_Z[3] = { 0.0f, 0.0f, 1.0f };
	static const float SPINE_SHARE[3] = { CHARACTER_SPINE_SHARE_1,
		                                  CHARACTER_SPINE_SHARE_2,
		                                  CHARACTER_SPINE_SHARE_3 };

	// +z is forward and +x is left: a roll about +z tips right, so lean left <0
	for(int i = 0; i < 3; i++) {
		rotate_bone(model, pose, rig->spine[i], AXIS_X,
		            offsets->lean_forward * SPINE_SHARE[i]);
		rotate_bone(model, pose, rig->spine[i], AXIS_Z,
		            -offsets->lean_left * SPINE_SHARE[i]);
		rotate_bone(model, pose, rig->spine[i], AXIS_Y,
		            offsets->spine_twist * SPINE_SHARE[i]);
	}

	rotate_bone(model, pose, rig->neck, AXIS_Y,
	            offsets->head_yaw * CHARACTER_NECK_YAW_SHARE);
	rotate_bone(model, pose, rig->head, AXIS_Y,
	            offsets->head_yaw * (1.0f - CHARACTER_NECK_YAW_SHARE));
}
