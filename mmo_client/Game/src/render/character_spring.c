// Spring bones: each bone's free end is simulated, then the bone turns to it

#include "render/character_spring.h"
#include "render/character_pose.h"
#include "render/character_tuning.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

// The longest one step is, seconds; a longer frame is split evenly
#define SPRING_STEP      (1.0f / 120.0f)
#define SPRING_MAX_STEPS 4
// A gap this long (seconds) means the body was not drawn: start over
#define SPRING_STALE_SEC 0.25f
// A body that moves this far (world units) in one update was moved: start over
#define SPRING_TELEPORT_DIST 100.0f
// A bone shorter than this (world units) has no direction worth swinging
#define SPRING_MIN_LENGTH 1e-4f
// How many times a step takes turns between the colliders and the bone length
#define SPRING_COLLIDE_PASSES 4
// Two directions this alike (cosine) need no turn between them
#define SPRING_SAME_DIRECTION 0.999999f
// The wind's shiver: times a second, and how far each joint is out of step
#define SPRING_FLUTTER_HZ 5.0f
#define SPRING_FLUTTER_SPREAD 0.137f

// How far along a body's state is (character_spring_state.stage)
enum { SPRING_UNPRIMED, SPRING_JUST_PRIMED, SPRING_RUNNING };

// The model-frame matrix of every node of the pose being swung
static Mat4 g_global[SKIN_MAX_NODES];
// The colliders in that pose, and those the joint being swung can reach
static float g_centers[SPRING_MAX_COLLIDERS][3];
static float g_normals[SPRING_MAX_COLLIDERS][3];
static float g_cluster_centers[SPRING_MAX_CLUSTERS][3];
static int g_nearby[SPRING_MAX_COLLIDERS];
static int g_nearby_count;

static int has_prefix(const char* name, const char* prefix) {
	return strncmp(name, prefix, strlen(prefix)) == 0;
}

// Give each joint the length of its chain, from the model as made
static void measure_chains(const Skeleton* skeleton, character_spring_rig* rig) {
	float total[SPRING_MAX_JOINTS] = { 0 };
	int root[SPRING_MAX_JOINTS];

	for(int j = 0; j < rig->count; j++) {
		const float* t = skeleton->bind_local[rig->joint[j].tip_node].translation;

		root[j] = rig->joint[j].parent < 0 ? j : root[rig->joint[j].parent];
		total[root[j]] += sqrtf(t[0] * t[0] + t[1] * t[1] + t[2] * t[2]);
	}
	for(int j = 0; j < rig->count; j++)
		rig->joint[j].chain = total[root[j]];
}

static float length3(const float v[3]);

// How far a joint's skin stands off its bone on average, as made, up to a limit
static void measure_thickness(const SkinnedModel* model, character_spring_rig* rig) {
	const Skeleton* skeleton = &model->skeleton;
	Mat4 global[SKIN_MAX_NODES];
	float sum[SPRING_MAX_JOINTS] = { 0 };
	int count[SPRING_MAX_JOINTS] = { 0 };
	int joint_of_node[SKIN_MAX_NODES];
	const float origin[3] = { 0.0f, 0.0f, 0.0f };

	for(int i = 0; i < skeleton->node_count; i++) {
		int n = skeleton->order[i];
		const Transform* t = &skeleton->bind_local[n];
		Mat4 matrix = mat4_from_trs(t->translation, t->rotation, t->scale);

		global[n] = skeleton->parent[n] < 0 ? matrix
		                                    : mat4_multiply(global[skeleton->parent[n]], matrix);
		joint_of_node[n] = -1;
	}
	for(int j = 0; j < rig->count; j++) {
		joint_of_node[rig->joint[j].node] = j;
		rig->joint[j].thickness = 0.0f;
	}

	for(int m = 0; m < model->mesh_count; m++) {
		const SkinnedMesh* mesh = &model->meshes[m];

		if(!mesh->weights || !mesh->joints)
			continue;
		for(int v = 0; v < mesh->vertex_count; v++) {
			int best = 0;
			int j;
			float a[3], b[3], axis[3], rel[3], along;

			for(int k = 1; k < SKIN_INFLUENCES; k++)
				if(mesh->weights[v * SKIN_INFLUENCES + k] > mesh->weights[v * SKIN_INFLUENCES + best])
					best = k;
			j = joint_of_node[skeleton->joint_node[mesh->joints[v * SKIN_INFLUENCES + best]]];
			if(j < 0)
				continue;

			mat4_transform_point(&global[rig->joint[j].node], origin, a);
			mat4_transform_point(&global[rig->joint[j].tip_node], origin, b);
			for(int i = 0; i < 3; i++) {
				axis[i] = b[i] - a[i];
				rel[i] = mesh->positions[v * 3 + i] - a[i];
			}
			along = (rel[0] * axis[0] + rel[1] * axis[1] + rel[2] * axis[2]) /
			        fmaxf(length3(axis) * length3(axis), 1e-9f);
			if(along < 0.0f || along > 1.0f)
				continue;
			for(int i = 0; i < 3; i++)
				rel[i] -= axis[i] * along;
			sum[j] += length3(rel);
			count[j]++;
		}
	}
	// Only a long braid is held clear by its thickness; short strands stay as made
	for(int j = 0; j < rig->count; j++)
		if(count[j] >= 4 && rig->joint[j].chain >= CHARACTER_HAIR_DRAPE_LENGTH)
			rig->joint[j].thickness = fminf(sum[j] / (float)count[j], CHARACTER_HAIR_CLEARANCE);
}

int character_spring_find(const SkinnedModel* model, const char* prefix,
                          character_spring_rig* rig) {
	const Skeleton* skeleton = &model->skeleton;
	int joint_of[SKIN_MAX_NODES];

	rig->count = 0;
	rig->collider_count = 0;
	for(int i = 0; i < skeleton->node_count; i++)
		joint_of[i] = -1;

	for(int i = 0; i < skeleton->node_count && rig->count < SPRING_MAX_JOINTS; i++) {
		int n = skeleton->order[i];
		int tip = -1;
		character_spring_joint* joint = &rig->joint[rig->count];

		if(!has_prefix(skeleton->node_names[n], prefix))
			continue;
		for(int c = 0; c < skeleton->node_count && tip < 0; c++)
			if(skeleton->parent[c] == n && has_prefix(skeleton->node_names[c], prefix))
				tip = c;
		if(tip < 0)
			continue;

		joint->node = n;
		joint->tip_node = tip;
		joint->parent = skeleton->parent[n] >= 0 ? joint_of[skeleton->parent[n]] : -1;
		joint_of[n] = rig->count;
		rig->count++;
	}
	measure_chains(skeleton, rig);
	measure_thickness(model, rig);

	return rig->count;
}

void character_spring_reset(character_spring_state* state) {
	memset(state, 0, sizeof(*state));
}

// A world direction as the model's frame sees it (rotation and one scale)
static void to_model_direction(const Mat4* to_world, const float v[3], float out[3]) {
	float scale_squared = to_world->m[0] * to_world->m[0] + to_world->m[1] * to_world->m[1] +
	                      to_world->m[2] * to_world->m[2];

	for(int r = 0; r < 3; r++)
		out[r] = (to_world->m[r * 4] * v[0] + to_world->m[r * 4 + 1] * v[1] +
		          to_world->m[r * 4 + 2] * v[2]) / scale_squared;
}

static float length3(const float v[3]) {
	return sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}

// Make g_global[node] from its parent's and its local transform in `pose`
static void refresh_global(const Skeleton* skeleton, const Pose* pose, int node) {
	const Transform* local = &pose->local[node];
	Mat4 matrix = mat4_from_trs(local->translation, local->rotation, local->scale);
	int parent = skeleton->parent[node];

	g_global[node] = parent < 0 ? matrix : mat4_multiply(g_global[parent], matrix);
}

static void refresh_all_globals(const Skeleton* skeleton, const Pose* pose) {
	for(int i = 0; i < skeleton->node_count; i++)
		refresh_global(skeleton, pose, skeleton->order[i]);
}

// One step of a free end, kept relative to its moving head
static void step_tip(const character_spring_params* params, const float rest[3],
                     const float head_velocity[3], const float extra[3], float length,
                     float max_speed, float h, float tip[3], float velocity[3]) {
	float drag = fminf(1.0f, params->drag * h);
	float next[3], relative[3];
	float distance, speed;

	for(int i = 0; i < 3; i++) {
		float pull = params->stiffness * (rest[i] - tip[i]) / length + extra[i];

		velocity[i] -= (velocity[i] - head_velocity[i]) * drag;
		if(i == 1)
			pull -= params->gravity;
		velocity[i] += pull * h;
		relative[i] = velocity[i] - head_velocity[i];
	}

	// Slow enough that a step cannot jump a collider
	speed = length3(relative);
	if(speed > max_speed)
		for(int i = 0; i < 3; i++)
			relative[i] *= max_speed / speed;
	for(int i = 0; i < 3; i++) {
		velocity[i] = relative[i] + head_velocity[i];
		next[i] = tip[i] + relative[i] * h;
	}

	// The bone does not stretch: the end stays its length from the head
	distance = length3(next);
	if(distance > 1e-6f)
		for(int i = 0; i < 3; i++)
			next[i] *= length / distance;

	for(int i = 0; i < 3; i++) {
		velocity[i] = (next[i] - tip[i]) / h + head_velocity[i];
		tip[i] = next[i];
	}
}

// Turn a bone so the way it points goes from `from` to `to` (model frame)
static void aim_bone(const SkinnedModel* model, Pose* pose, int node, const float from[3],
                     const float to[3]) {
	float a = length3(from);
	float b = length3(to);
	float unit_from[3], unit_to[3], turn[4];
	float dot;

	if(a < SPRING_MIN_LENGTH || b < SPRING_MIN_LENGTH)
		return;

	for(int i = 0; i < 3; i++) {
		unit_from[i] = from[i] / a;
		unit_to[i] = to[i] / b;
	}
	dot = unit_from[0] * unit_to[0] + unit_from[1] * unit_to[1] + unit_from[2] * unit_to[2];
	if(dot > SPRING_SAME_DIRECTION)
		return;

	quat_between(unit_from, unit_to, turn);
	character_pose_turn_node(model, pose, node, turn);
}

// What every joint of a rig shares in one update
typedef struct {
	const SkinnedModel* model;
	const character_spring_rig* rig;
	const character_spring_params* params;
	const Mat4* to_world;
	float scale;   // world units per model unit
	float dt;      // seconds since the last update
	float sim_dt;  // the part of it that is simulated
	int stage;
} spring_frame;

// Move `tip` by `delta` (model frame) in the world's terms
static void shift_tip(const spring_frame* frame, const float delta[3], float tip[3]) {
	float moved[3];

	mat4_transform_direction(frame->to_world, delta, moved);
	for(int i = 0; i < 3; i++)
		tip[i] += moved[i];
}

// Push a bone's end and middle a thickness clear of the colliders; 1 if moved
static int push_bone_out(const spring_frame* frame, int joint, const float head_model[3],
                         float tip[3]) {
	static const float ALONG[2] = { 1.0f, 0.5f };
	float clearance = frame->rig->joint[joint].thickness;
	int moved = 0;

	for(int p = 0; p < 2; p++) {
		float local[3], point[3], pushed[3], delta[3];

		to_model_direction(frame->to_world, tip, local);
		for(int i = 0; i < 3; i++)
			point[i] = pushed[i] = head_model[i] + local[i] * ALONG[p];
		if(!character_spring_push_out(frame->rig, joint, g_centers, g_normals, g_nearby,
		                              g_nearby_count, clearance, pushed))
			continue;

		for(int i = 0; i < 3; i++)
			delta[i] = (pushed[i] - point[i]) / ALONG[p];
		shift_tip(frame, delta, tip);
		moved = 1;
	}

	return moved;
}

// Keep a free end out of the body and its bone its length, by turns
static void keep_out(const spring_frame* frame, int joint, const float head_model[3],
                     float length, float tip[3]) {
	if(frame->rig->collider_count == 0)
		return;

	for(int pass = 0; pass < SPRING_COLLIDE_PASSES; pass++) {
		int moved = push_bone_out(frame, joint, head_model, tip);
		float distance = length3(tip);

		if(distance > 1e-6f)
			for(int i = 0; i < 3; i++)
				tip[i] *= length / distance;
		if(!moved)
			break;
	}
}

// The pull drawing a braid's end onto the nearest body, as a world acceleration
static void press_toward_body(const spring_frame* frame, const character_spring_params* params,
                              float thickness, const float head_model[3], const float tip[3],
                              float out[3]) {
	float local[3], point[3], toward[3], world[3];
	float nearest = params->drape_reach;
	int found = -1;

	out[0] = out[1] = out[2] = 0.0f;
	if(params->drape_press <= 0.0f)
		return;

	to_model_direction(frame->to_world, tip, local);
	for(int i = 0; i < 3; i++)
		point[i] = head_model[i] + local[i];
	for(int n = 0; n < g_nearby_count; n++) {
		const float* c = g_centers[g_nearby[n]];
		float d[3] = { c[0] - point[0], c[1] - point[1], c[2] - point[2] };
		float off = length3(d) - frame->rig->collider[g_nearby[n]].radius - thickness;

		if(frame->rig->collider[g_nearby[n]].blocks_only)
			continue;
		if(off < nearest) {
			nearest = off;
			found = n;
			memcpy(toward, d, sizeof(toward));
		}
	}
	if(found < 0 || length3(toward) < 1e-6f)
		return;

	mat4_transform_direction(frame->to_world, toward, world);
	for(int i = 0; i < 3; i++)
		out[i] = world[i] / length3(world) * params->drape_press;
}

// The fastest an end moves against its head, rig units a second; huge for none
static float speed_limit(const character_spring_params* params) {
	if(params->max_speed < 0.0f)
		return 1e9f;

	return params->max_speed > 0.0f ? params->max_speed : CHARACTER_SPRING_MAX_SPEED;
}

// The wind's pull on a joint's end, a world acceleration, shivering by joint
static void wind_push(const character_spring_params* params, int joint, float out[3]) {
	float cycles = params->wind_time * SPRING_FLUTTER_HZ + (float)joint * SPRING_FLUTTER_SPREAD;
	float shiver = 1.0f + params->wind_flutter * sinf(6.2831853f * (cycles - floorf(cycles)));

	for(int i = 0; i < 3; i++)
		out[i] = params->wind[i] * shiver * params->stiffness;
}

// Swing one joint: move its free end, then turn its bone to the new end
static void swing_joint(const spring_frame* frame, int j, character_spring_state* state,
                        Pose* pose) {
	const character_spring_joint* joint = &frame->rig->joint[j];
	const float* tip_offset = pose->local[joint->tip_node].translation;
	float* tip = state->tip[j];
	float* velocity = state->velocity[j];
	float* last_head = state->head[j];
	float length = length3(tip_offset) * frame->scale;
	int steps = (int)ceilf(frame->sim_dt / SPRING_STEP);
	float head_model[3], rest_model[3], head[3], rest[3], head_velocity[3] = { 0.0f, 0.0f, 0.0f };
	float bone_now[3], bone_new[3], blown[3];
	character_spring_params params = *frame->params;

	// A braid droops: softer toward its made pose, with the full pull of gravity
	if(params.drape_length > 0.0f && joint->chain >= params.drape_length) {
		params.stiffness = params.drape_stiffness;
		params.gravity = params.drape_gravity;
	} else {
		params.drape_press = 0.0f;
		params.drape_reach = 0.0f;
	}

	// The bone above may have turned since the matrices were made
	refresh_global(&frame->model->skeleton, pose, joint->node);
	memcpy(head_model, &g_global[joint->node].m[12], sizeof(head_model));
	mat4_transform_point(&g_global[joint->node], tip_offset, rest_model);
	mat4_transform_point(frame->to_world, head_model, head);
	mat4_transform_point(frame->to_world, rest_model, rest);
	for(int i = 0; i < 3; i++)
		rest[i] -= head[i];

	g_nearby_count = params.skip_colliders ? 0 :
	                 character_spring_nearby_colliders(frame->rig, g_centers, g_cluster_centers,
	                                                   head_model, length / frame->scale +
	                                                   params.drape_reach + joint->thickness,
	                                                   g_nearby);
	if(frame->stage == SPRING_UNPRIMED || length < SPRING_MIN_LENGTH) {
		memcpy(tip, rest, sizeof(rest));
		memset(velocity, 0, 3 * sizeof(float));
		if(length >= SPRING_MIN_LENGTH)
			keep_out(frame, j, head_model, length, tip);
	} else {
		for(int i = 0; i < 3; i++)
			head_velocity[i] = frame->dt > 1e-6f ? (head[i] - last_head[i]) / frame->dt : 0.0f;
		if(frame->stage == SPRING_JUST_PRIMED)
			memcpy(velocity, head_velocity, sizeof(head_velocity));
		wind_push(&params, j, blown);
		for(int s = 0; s < steps; s++) {
			float extra[3];

			press_toward_body(frame, &params, joint->thickness, head_model, tip, extra);
			for(int i = 0; i < 3; i++)
				extra[i] += blown[i];
			step_tip(&params, rest, head_velocity, extra, length,
			         speed_limit(&params) * frame->scale, frame->sim_dt / (float)steps, tip,
			         velocity);
			keep_out(frame, j, head_model, length, tip);
		}
	}
	memcpy(last_head, head, sizeof(head));

	to_model_direction(frame->to_world, tip, bone_new);
	for(int i = 0; i < 3; i++)
		bone_now[i] = rest_model[i] - head_model[i];
	aim_bone(frame->model, pose, joint->node, bone_now, bone_new);
	refresh_global(&frame->model->skeleton, pose, joint->node);
}

void character_spring_update(const SkinnedModel* model, const character_spring_rig* rig,
                             const character_spring_params* params, const Mat4* to_world,
                             float dt, character_spring_state* state, Pose* pose) {
	// Around the body, not the world's origin: far out, floats lose small bones
	Mat4 around = *to_world;
	float origin[3] = { to_world->m[12], to_world->m[13], to_world->m[14] };
	float shift[3] = { state->origin[0] - origin[0], state->origin[1] - origin[1],
	                   state->origin[2] - origin[2] };
	spring_frame frame = { model, rig, params, &around, length3(to_world->m), dt,
	                       fminf(fmaxf(dt, 0.0f), SPRING_STEP * SPRING_MAX_STEPS), state->stage };

	if(rig->count == 0 || frame.scale < SPRING_MIN_LENGTH)
		return;

	if(dt > SPRING_STALE_SEC || length3(shift) > SPRING_TELEPORT_DIST)
		frame.stage = SPRING_UNPRIMED;
	around.m[12] = around.m[13] = around.m[14] = 0.0f;
	refresh_all_globals(&model->skeleton, pose);
	character_spring_collider_centers(rig, g_global, g_centers, g_normals, g_cluster_centers);
	for(int j = 0; j < rig->count; j++) {
		if(frame.stage != SPRING_UNPRIMED)
			for(int i = 0; i < 3; i++)
				state->head[j][i] += shift[i];
		swing_joint(&frame, j, state, pose);
	}

	memcpy(state->origin, origin, sizeof(origin));
	state->stage = frame.stage == SPRING_UNPRIMED ? SPRING_JUST_PRIMED : SPRING_RUNNING;
}
