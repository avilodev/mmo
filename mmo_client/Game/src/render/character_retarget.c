// Plays the kit's clips on a model that shares the kit's bone names

#include "render/character_retarget.h"
#include "render/character_pose.h"
#include "render/character_tuning.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define PI 3.14159265358979323846f

// Bones that follow the kit's translation as well as its rotation
static const char* const MOVED_BONES[] = { "root", "pelvis" };

#define MOVED_BONE_COUNT ((int)(sizeof(MOVED_BONES) / sizeof(MOVED_BONES[0])))

typedef struct {
	const char* name;
	float counter;
} twist_slot;

// The forearm twist bones the model's maker adds, per side
static const twist_slot TWIST_SLOTS[] = {
	{ "lowerarm_twist_01", CHARACTER_TWIST_01_COUNTER },
	{ "lowerarm_twist_02", CHARACTER_TWIST_02_COUNTER },
};

#define TWIST_SLOT_COUNT ((int)(sizeof(TWIST_SLOTS) / sizeof(TWIST_SLOTS[0])))

// Whether a node of the skeleton is one of its joints
static int is_joint(const Skeleton* skeleton, int node) {
	for(int j = 0; j < skeleton->joint_count; j++)
		if(skeleton->joint_node[j] == node)
			return 1;

	return 0;
}

static float length(const float v[3]) {
	return sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}

// Match each joint of the target to the kit bone of the same name
static int find_copies(const SkinnedModel* kit, const SkinnedModel* target,
                       character_retarget* out) {
	const Skeleton* skeleton = &target->skeleton;
	int copied = 0;

	for(int i = 0; i < skeleton->node_count; i++) {
		out->source[i] = -1;
		if(skeleton->node_names[i][0] == '\0' || !is_joint(skeleton, i))
			continue;

		out->source[i] = character_pose_find_node(kit, skeleton->node_names[i]);
		if(out->source[i] >= 0)
			copied++;
	}

	return copied;
}

// Find the bones that move, and how much taller the target's pelvis stands
static void find_moved(const SkinnedModel* kit, const SkinnedModel* target,
                       character_retarget* out) {
	int pelvis_kit = character_pose_find_node(kit, "pelvis");
	int pelvis_target = character_pose_find_node(target, "pelvis");

	for(int b = 0; b < MOVED_BONE_COUNT; b++) {
		int node = character_pose_find_node(target, MOVED_BONES[b]);
		int source = character_pose_find_node(kit, MOVED_BONES[b]);

		if(node < 0 || source < 0)
			continue;

		out->moved_node[out->moved_count] = node;
		out->moved_source[out->moved_count] = source;
		out->moved_count++;
	}

	out->move_scale = 1.0f;
	if(pelvis_kit < 0 || pelvis_target < 0)
		return;

	float kit_height = length(kit->skeleton.bind_local[pelvis_kit].translation);
	float target_height = length(target->skeleton.bind_local[pelvis_target].translation);

	if(kit_height > 1e-4f && target_height > 1e-4f)
		out->move_scale = target_height / kit_height;
}

// Find the twist bones of each forearm
static void find_twists(const SkinnedModel* kit, const SkinnedModel* target,
                        character_retarget* out) {
	static const char SIDES[] = { 'l', 'r' };

	for(int s = 0; s < 2; s++) {
		char name[SKIN_NAME_MAX];

		snprintf(name, sizeof(name), "lowerarm_%c", SIDES[s]);

		int source = character_pose_find_node(kit, name);

		if(source < 0)
			continue;

		for(int t = 0; t < TWIST_SLOT_COUNT && out->twist_count < RETARGET_TWIST_MAX; t++) {
			snprintf(name, sizeof(name), "%s_%c", TWIST_SLOTS[t].name, SIDES[s]);

			int node = character_pose_find_node(target, name);

			if(node < 0)
				continue;

			out->twist[out->twist_count].node = node;
			out->twist[out->twist_count].source = source;
			out->twist[out->twist_count].counter = TWIST_SLOTS[t].counter;
			out->twist_count++;
		}
	}
}

int character_retarget_build(const SkinnedModel* kit, const SkinnedModel* target,
                             character_retarget* out) {
	memset(out, 0, sizeof(*out));
	out->move_scale = 1.0f;

	int copied = find_copies(kit, target, out);

	if(copied == 0)
		return 0;

	find_moved(kit, target, out);
	find_twists(kit, target, out);

	return copied;
}

float character_retarget_roll(const float rotation[4]) {
	// The twist of a swing-twist split about the local y axis
	float roll = 2.0f * atan2f(rotation[1], rotation[3]);

	if(roll > PI)
		roll -= 2.0f * PI;
	else if(roll <= -PI)
		roll += 2.0f * PI;

	return roll;
}

// Turn a twist bone back against the kit forearm's roll past the dead zone
static void counter_roll(const character_retarget* retarget, const SkinnedModel* kit,
                         const Pose* kit_pose, int index, Pose* out) {
	static const float AXIS_Y[3] = { 0.0f, 1.0f, 0.0f };
	const retarget_twist* twist = &retarget->twist[index];
	const float* rest = kit->skeleton.bind_local[twist->source].rotation;
	const float inverse[4] = { -rest[0], -rest[1], -rest[2], rest[3] };
	float delta[4];
	float turn[4];
	float result[4];

	quat_multiply(inverse, kit_pose->local[twist->source].rotation, delta);

	float roll = character_retarget_roll(delta);
	float excess = fabsf(roll) - CHARACTER_TWIST_DEADZONE_DEG * PI / 180.0f;

	if(excess <= 0.0f)
		return;

	quat_from_axis_angle(AXIS_Y, (roll > 0.0f ? -1.0f : 1.0f) * twist->counter * excess, turn);
	quat_multiply(out->local[twist->node].rotation, turn, result);
	memcpy(out->local[twist->node].rotation, result, sizeof(result));
}

void character_retarget_apply(const character_retarget* retarget, const SkinnedModel* kit,
                              const SkinnedModel* target, const Pose* kit_pose, Pose* out) {
	skinned_model_bind_pose(target, out);

	for(int i = 0; i < target->skeleton.node_count; i++)
		if(retarget->source[i] >= 0)
			memcpy(out->local[i].rotation, kit_pose->local[retarget->source[i]].rotation,
			       sizeof(out->local[i].rotation));

	// The kit's move away from its rest, as tall as this model stands
	for(int m = 0; m < retarget->moved_count; m++) {
		const float* rest = kit->skeleton.bind_local[retarget->moved_source[m]].translation;
		const float* now = kit_pose->local[retarget->moved_source[m]].translation;
		float* at = out->local[retarget->moved_node[m]].translation;

		for(int c = 0; c < 3; c++)
			at[c] += (now[c] - rest[c]) * retarget->move_scale;
	}

	for(int t = 0; t < retarget->twist_count; t++)
		counter_roll(retarget, kit, kit_pose, t, out);
}
