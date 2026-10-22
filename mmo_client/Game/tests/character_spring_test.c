// Spring bones: the players' hair trails the body and settles

#include "model/skinned_model.h"
#include "render/character_spring.h"
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

#define HAIR_PREFIX CHARACTER_HAIR_PREFIX
#define TAIL_PREFIX CHARACTER_TAIL_PREFIX
#define PLAYER_SCALE 22.0f

static SkinnedModel g_male, g_female, g_kit;
static Pose g_rest, g_pose;
// The settings run_along_z swings with: the hair's unless a test says otherwise
static character_spring_params g_params;
// The axis run_along_z moves the body along: 2 for z, 0 for x
static int g_axis = 2;

// The matrix a body is drawn with: moved, turned and scaled
static Mat4 body_matrix(float x, float y, float z, float yaw) {
	Mat4 move = mat4_translation(x, y, z);
	Mat4 turn = mat4_rotation_degrees(yaw, 0.0f, 1.0f, 0.0f);
	Mat4 grow = mat4_scaling(PLAYER_SCALE, PLAYER_SCALE, PLAYER_SCALE);

	return mat4_multiply(move, mat4_multiply(turn, grow));
}

// Where a node's joint sits in the model's frame
static void node_position(const SkinnedModel* model, const Pose* pose, int node, float out[3]) {
	const Skeleton* skeleton = &model->skeleton;
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

static float length3(const float v[3]) {
	return sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}

// The angle between the direction a spring bone points in two poses, radians
static float bone_angle(const SkinnedModel* model, const character_spring_joint* joint,
                        const Pose* a, const Pose* b) {
	float head_a[3], tip_a[3], head_b[3], tip_b[3], da[3], db[3];
	float dot;

	node_position(model, a, joint->node, head_a);
	node_position(model, a, joint->tip_node, tip_a);
	node_position(model, b, joint->node, head_b);
	node_position(model, b, joint->tip_node, tip_b);
	for(int i = 0; i < 3; i++) {
		da[i] = tip_a[i] - head_a[i];
		db[i] = tip_b[i] - head_b[i];
	}
	dot = (da[0] * db[0] + da[1] * db[1] + da[2] * db[2]) / (length3(da) * length3(db));

	return acosf(fminf(1.0f, fmaxf(-1.0f, dot)));
}

// The most any spring bone of `pose` is turned away from `rest`
static float worst_angle(const SkinnedModel* model, const character_spring_rig* rig,
                         const Pose* pose, const Pose* rest) {
	float worst = 0.0f;

	for(int j = 0; j < rig->count; j++) {
		float angle = bone_angle(model, &rig->joint[j], pose, rest);

		if(angle > worst)
			worst = angle;
	}

	return worst;
}

// The most any joint outside the drooping braids is turned away from `rest`
static float worst_unbraided(const SkinnedModel* model, const character_spring_rig* rig,
                             const Pose* pose, const Pose* rest) {
	float worst = 0.0f;

	for(int j = 0; j < rig->count; j++) {
		float angle;

		if(rig->joint[j].chain >= CHARACTER_HAIR_DRAPE_LENGTH)
			continue;
		angle = bone_angle(model, &rig->joint[j], pose, rest);
		if(angle > worst)
			worst = angle;
	}

	return worst;
}

static int pose_has_nan(const SkinnedModel* model, const Pose* pose) {
	for(int i = 0; i < model->skeleton.node_count; i++)
		for(int c = 0; c < 4; c++)
			if(!isfinite(pose->local[i].rotation[c]))
				return 1;

	return 0;
}

static character_spring_params hair_params(void) {
	character_spring_params params = { .stiffness = CHARACTER_HAIR_STIFFNESS,
	                                   .drag = CHARACTER_HAIR_DRAG,
	                                   .gravity = CHARACTER_HAIR_GRAVITY,
	                                   .drape_length = CHARACTER_HAIR_DRAPE_LENGTH,
	                                   .drape_stiffness = CHARACTER_HAIR_DRAPE_STIFFNESS,
	                                   .drape_gravity = CHARACTER_HAIR_DRAPE_GRAVITY,
	                                   .drape_press = CHARACTER_HAIR_DRAPE_PRESS,
	                                   .drape_reach = CHARACTER_HAIR_DRAPE_REACH };

	return params;
}

// Run the body along z at `accel` for `seconds`; returns the end speed
static float run_along_z(const SkinnedModel* model, const character_spring_rig* rig,
                         character_spring_state* state, float* z, float speed, float accel,
                         float seconds, float dt) {
	character_spring_params params = g_params;

	for(float t = 0.0f; t < seconds; t += dt) {
		Mat4 matrix;

		speed += accel * dt;
		*z += speed * dt;
		matrix = g_axis == 0 ? body_matrix(*z, 0.0f, 0.0f, 0.0f) : body_matrix(0.0f, 0.0f, *z, 0.0f);
		g_pose = g_rest;
		character_spring_update(model, rig, &params, &matrix, dt, state, &g_pose);
	}

	return speed;
}

static character_spring_params tail_params(void) {
	character_spring_params params = { .stiffness = CHARACTER_TAIL_STIFFNESS,
	                                   .drag = CHARACTER_TAIL_DRAG,
	                                   .gravity = CHARACTER_TAIL_GRAVITY };

	return params;
}

static int count_joints(const SkinnedModel* model, const char* prefix) {
	const Skeleton* skeleton = &model->skeleton;
	int count = 0;

	for(int n = 0; n < skeleton->node_count; n++) {
		int has_child = 0;

		if(strncmp(skeleton->node_names[n], prefix, strlen(prefix)) != 0)
			continue;
		for(int c = 0; c < skeleton->node_count; c++)
			if(skeleton->parent[c] == n &&
			   strncmp(skeleton->node_names[c], prefix, strlen(prefix)) == 0)
				has_child = 1;
		count += has_child;
	}

	return count;
}

static void test_rig(void) {
	static character_spring_rig rig_male, rig_female, rig_kit;
	int parents_first = 1;

	printf("\nTEST 1: the rig finds every hair bone that has a bone below it\n");
	CHECK(character_spring_find(&g_male, HAIR_PREFIX, &rig_male) == count_joints(&g_male, HAIR_PREFIX),
	      "the male model's joints are all the hair bones with a hair bone below");
	CHECK(rig_male.count == 39, "the male model has 13 chains of 3 joints");
	CHECK(character_spring_find(&g_female, HAIR_PREFIX, &rig_female) == count_joints(&g_female, HAIR_PREFIX),
	      "the female model's joints are all the hair bones with a hair bone below");
	CHECK(character_spring_find(&g_kit, HAIR_PREFIX, &rig_kit) == 0, "the kit has no hair");
	for(int j = 0; j < rig_male.count; j++) {
		const character_spring_joint* joint = &rig_male.joint[j];

		if(joint->parent >= j)
			parents_first = 0;
		if(g_male.skeleton.parent[joint->tip_node] != joint->node)
			parents_first = 0;
	}
	CHECK(parents_first, "a joint's parent joint comes before it and its tip hangs off it");
	CHECK(rig_male.joint[0].parent == -1, "the first joint hangs off a plain bone");
}

static void test_rest(void) {
	static character_spring_rig rig;
	static character_spring_state state;
	character_spring_params params = hair_params();
	Mat4 matrix = body_matrix(100.0f, 5.0f, 200.0f, 30.0f);

	printf("\nTEST 2: a body that stands still keeps its hair where it was made\n");
	character_spring_find(&g_male, HAIR_PREFIX, &rig);
	character_spring_reset(&state);
	g_pose = g_rest;
	character_spring_update(&g_male, &rig, &params, &matrix, 1.0f / 60.0f, &state, &g_pose);
	CHECK(worst_angle(&g_male, &rig, &g_pose, &g_rest) < 1e-3f, "the first update leaves the pose");
	for(int i = 0; i < 240; i++) {
		g_pose = g_rest;
		character_spring_update(&g_male, &rig, &params, &matrix, 1.0f / 60.0f, &state, &g_pose);
	}
	CHECK(worst_angle(&g_male, &rig, &g_pose, &g_rest) < 0.12f,
	      "four seconds standing: hair hangs within 7 degrees of its rest");
	CHECK(!pose_has_nan(&g_male, &g_pose), "no NaN");
}

static void test_trail(void) {
	static character_spring_rig rig;
	static character_spring_state state;
	float rest_tip[3], tip[3];
	const character_spring_joint* joint;
	float z = 0.0f;
	float speed;

	printf("\nTEST 3: hair trails behind a body that speeds up, and settles after it stops\n");
	character_spring_find(&g_male, HAIR_PREFIX, &rig);
	character_spring_reset(&state);
	run_along_z(&g_male, &rig, &state, &z, 0.0f, 0.0f, 0.5f, 1.0f / 60.0f);
	speed = run_along_z(&g_male, &rig, &state, &z, 0.0f, 1500.0f, 0.15f, 1.0f / 60.0f);
	joint = &rig.joint[0];
	node_position(&g_male, &g_rest, joint->tip_node, rest_tip);
	node_position(&g_male, &g_pose, joint->tip_node, tip);
	CHECK(speed > 100.0f, "the body is running");
	CHECK(tip[2] < rest_tip[2] - 0.002f, "the first hair bone's tip lags behind the way it runs");
	CHECK(worst_angle(&g_male, &rig, &g_pose, &g_rest) < 2.0f, "no bone folds back past 115 degrees");

	run_along_z(&g_male, &rig, &state, &z, speed, 0.0f, 1.0f, 1.0f / 60.0f);
	CHECK(worst_angle(&g_male, &rig, &g_pose, &g_rest) < 0.3f,
	      "running steadily the hair has caught up to within 17 degrees");

	run_along_z(&g_male, &rig, &state, &z, 0.0f, 0.0f, 1.5f, 1.0f / 60.0f);
	CHECK(worst_angle(&g_male, &rig, &g_pose, &g_rest) < 0.12f,
	      "1.5 s after stopping the hair hangs within 7 degrees of its rest");
}

static void test_bones(void) {
	static character_spring_rig rig;
	static character_spring_state state;
	float z = 0.0f;
	int kept = 1;

	printf("\nTEST 4: only rotations change, never a bone's length or place\n");
	character_spring_find(&g_male, HAIR_PREFIX, &rig);
	character_spring_reset(&state);
	run_along_z(&g_male, &rig, &state, &z, 0.0f, 0.0f, 0.2f, 1.0f / 60.0f);
	run_along_z(&g_male, &rig, &state, &z, 0.0f, 1500.0f, 0.15f, 1.0f / 60.0f);
	for(int n = 0; n < g_male.skeleton.node_count; n++)
		for(int c = 0; c < 3; c++) {
			if(g_pose.local[n].translation[c] != g_rest.local[n].translation[c])
				kept = 0;
			if(g_pose.local[n].scale[c] != g_rest.local[n].scale[c])
				kept = 0;
		}
	CHECK(kept, "translations and scales are as made");
}

static void test_bodies(void) {
	static character_spring_rig rig;
	static character_spring_state state;
	character_spring_params params = hair_params();
	Mat4 matrix;
	float z = 0.0f;

	printf("\nTEST 5: a teleport or a long gap re-primes instead of flinging the hair\n");
	character_spring_find(&g_male, HAIR_PREFIX, &rig);
	character_spring_reset(&state);
	run_along_z(&g_male, &rig, &state, &z, 0.0f, 0.0f, 0.3f, 1.0f / 60.0f);
	matrix = body_matrix(5000.0f, 0.0f, 9000.0f, 0.0f);
	g_pose = g_rest;
	character_spring_update(&g_male, &rig, &params, &matrix, 1.0f / 60.0f, &state, &g_pose);
	CHECK(worst_angle(&g_male, &rig, &g_pose, &g_rest) < 1e-3f, "a jump of thousands of units");
	matrix = body_matrix(5001.0f, 0.0f, 9000.0f, 0.0f);
	g_pose = g_rest;
	character_spring_update(&g_male, &rig, &params, &matrix, 5.0f, &state, &g_pose);
	CHECK(worst_angle(&g_male, &rig, &g_pose, &g_rest) < 1e-3f, "five seconds since the last update");

	printf("\nTEST 6: it stays stable at any frame rate\n");
	for(int k = 0; k < 3; k++) {
		static const float DT[3] = { 1.0f / 30.0f, 1.0f / 144.0f, 0.1f };
		float speed;

		z = 0.0f;
		character_spring_reset(&state);
		run_along_z(&g_male, &rig, &state, &z, 0.0f, 0.0f, 0.3f, DT[k]);
		speed = run_along_z(&g_male, &rig, &state, &z, 0.0f, 2500.0f, 0.2f, DT[k]);
		run_along_z(&g_male, &rig, &state, &z, speed, -2500.0f, 0.4f, DT[k]);
		CHECK(!pose_has_nan(&g_male, &g_pose), "no NaN");
		CHECK(worst_angle(&g_male, &rig, &g_pose, &g_rest) < 2.5f, "bones stay out of a full fold");
	}

	printf("\nTEST 7: turning on the spot swings the hair out and back\n");
	character_spring_reset(&state);
	{
		float worst = 0.0f;

		for(int i = 0; i < 90; i++) {
			float yaw = i < 20 ? (float)i * 12.0f : 240.0f;

			matrix = body_matrix(0.0f, 0.0f, 0.0f, yaw);
			g_pose = g_rest;
			character_spring_update(&g_male, &rig, &params, &matrix, 1.0f / 60.0f, &state, &g_pose);
			if(i == 25)
				worst = worst_angle(&g_male, &rig, &g_pose, &g_rest);
		}
		CHECK(worst > 0.02f, "hair lags a quick turn");
		CHECK(worst_angle(&g_male, &rig, &g_pose, &g_rest) < 0.12f, "and hangs again after it");
	}
}

static void test_female(void) {
	static character_spring_rig rig;
	static character_spring_state state;
	float z = 0.0f;
	float speed;

	printf("\nTEST 8: the female model's longer chains work too\n");
	character_spring_find(&g_female, HAIR_PREFIX, &rig);
	character_spring_reset(&state);
	skinned_model_bind_pose(&g_female, &g_rest);
	run_along_z(&g_female, &rig, &state, &z, 0.0f, 0.0f, 0.3f, 1.0f / 60.0f);
	speed = run_along_z(&g_female, &rig, &state, &z, 0.0f, 1500.0f, 0.15f, 1.0f / 60.0f);
	CHECK(worst_angle(&g_female, &rig, &g_pose, &g_rest) > 0.02f, "the hair moves");
	run_along_z(&g_female, &rig, &state, &z, speed, 0.0f, 1.0f, 1.0f / 60.0f);
	run_along_z(&g_female, &rig, &state, &z, 0.0f, 0.0f, 1.5f, 1.0f / 60.0f);
	CHECK(worst_unbraided(&g_female, &rig, &g_pose, &g_rest) < 0.12f,
	      "and the hair, braids aside, settles");
	CHECK(!pose_has_nan(&g_female, &g_pose), "no NaN");
}

static void test_tail(void) {
	static character_spring_rig rig, rig_female;
	static character_spring_state state;
	float rest_tip[3], tip[3];
	float z = 0.0f;
	float speed;

	printf("\nTEST 9: the tail swings like the hair, with its own weight\n");
	CHECK(character_spring_find(&g_male, TAIL_PREFIX, &rig) == count_joints(&g_male, TAIL_PREFIX),
	      "the male tail's joints are all its bones with a tail bone below");
	CHECK(rig.count == 6, "the male tail has 6 joints");
	CHECK(character_spring_find(&g_female, TAIL_PREFIX, &rig_female) == 6, "so has the female");
	CHECK(character_spring_find(&g_kit, TAIL_PREFIX, &rig_female) == 0, "the kit has none");

	// The tail lies along the way the body faces: a sideways run is what bends it
	g_params = tail_params();
	g_axis = 0;
	character_spring_find(&g_male, TAIL_PREFIX, &rig);
	character_spring_reset(&state);
	run_along_z(&g_male, &rig, &state, &z, 0.0f, 0.0f, 0.5f, 1.0f / 60.0f);
	speed = run_along_z(&g_male, &rig, &state, &z, 0.0f, 900.0f, 0.15f, 1.0f / 60.0f);
	node_position(&g_male, &g_rest, rig.joint[2].tip_node, rest_tip);
	node_position(&g_male, &g_pose, rig.joint[2].tip_node, tip);
	CHECK(tip[0] < rest_tip[0] - 0.005f, "the tail lags behind a sideways run");
	CHECK(worst_angle(&g_male, &rig, &g_pose, &g_rest) < 1.6f, "no tail bone folds past a right angle");
	run_along_z(&g_male, &rig, &state, &z, speed, 0.0f, 1.0f, 1.0f / 60.0f);
	run_along_z(&g_male, &rig, &state, &z, 0.0f, 0.0f, 2.0f, 1.0f / 60.0f);
	CHECK(worst_angle(&g_male, &rig, &g_pose, &g_rest) < 0.15f,
	      "2 s after stopping the tail hangs within 9 degrees of its rest");

	for(int k = 0; k < 3; k++) {
		static const float DT[3] = { 1.0f / 30.0f, 1.0f / 144.0f, 0.1f };

		z = 0.0f;
		character_spring_reset(&state);
		run_along_z(&g_male, &rig, &state, &z, 0.0f, 0.0f, 0.3f, DT[k]);
		speed = run_along_z(&g_male, &rig, &state, &z, 0.0f, 2500.0f, 0.2f, DT[k]);
		run_along_z(&g_male, &rig, &state, &z, speed, -2500.0f, 0.4f, DT[k]);
		CHECK(!pose_has_nan(&g_male, &g_pose), "no NaN");
	}
	g_axis = 2;
	g_params = hair_params();
}

// Stand a body still for `seconds` in a wind that leans its hair by `lean`; the
// first joint's tip is left in `tip`, and the worst bone turn away from rest returned
static float stand_in_wind(const character_spring_rig* rig, const float lean[3], float flutter,
                           float seconds, float tip[3]) {
	static character_spring_state state;
	character_spring_params params = hair_params();
	Mat4 matrix = body_matrix(0.0f, 0.0f, 0.0f, 0.0f);
	float dt = 1.0f / 60.0f;

	character_spring_reset(&state);
	memcpy(params.wind, lean, sizeof(params.wind));
	params.wind_flutter = flutter;
	for(float t = 0.0f; t < seconds; t += dt) {
		params.wind_time = t;
		g_pose = g_rest;
		character_spring_update(&g_male, rig, &params, &matrix, dt, &state, &g_pose);
	}
	node_position(&g_male, &g_pose, rig->joint[2].tip_node, tip);

	return worst_angle(&g_male, rig, &g_pose, &g_rest);
}

static void test_wind(void) {
	static character_spring_rig rig;
	static const float NONE[3] = { 0.0f, 0.0f, 0.0f };
	static const float BREEZE[3] = { 0.05f, 0.0125f, 0.0f };
	static const float GALE[3] = { 0.45f, 0.11f, 0.0f };
	float rest[3], calm[3], breeze[3], gale[3], shivering[3];
	float calm_turn, gale_turn;

	printf("\nTEST 10: the wind leans hair along the wind, and it settles when it drops\n");
	character_spring_find(&g_male, HAIR_PREFIX, &rig);
	node_position(&g_male, &g_rest, rig.joint[2].tip_node, rest);
	calm_turn = stand_in_wind(&rig, NONE, 0.0f, 3.0f, calm);
	CHECK(fabsf(calm[0] - rest[0]) < 0.005f, "no wind leaves the hair where it was");
	stand_in_wind(&rig, BREEZE, 0.0f, 3.0f, breeze);
	gale_turn = stand_in_wind(&rig, GALE, 0.0f, 3.0f, gale);
	printf("       breeze moves the tip %.1f cm, gale %.1f cm; gale turns the worst bone %.0f degrees\n",
	       (breeze[0] - calm[0]) * 100.0f, (gale[0] - calm[0]) * 100.0f, gale_turn * 57.2958f);
	CHECK(breeze[0] > calm[0] + 0.001f, "a breeze leans the hair downwind (+x)");
	CHECK(gale[0] > breeze[0] + 0.005f, "a gale leans it much further");
	CHECK(gale_turn > calm_turn + 0.1f && gale_turn < 1.6f,
	      "a gale turns bones noticeably, none folds past a right angle");
	stand_in_wind(&rig, GALE, 0.3f, 3.0f, shivering);
	CHECK(fabsf(shivering[0] - gale[0]) > 1e-5f, "a shiver moves the hair about its lean");
	CHECK(!pose_has_nan(&g_male, &g_pose), "no NaN");

	{
		static character_spring_state state;
		character_spring_params params = hair_params();
		Mat4 matrix = body_matrix(0.0f, 0.0f, 0.0f, 0.0f);

		character_spring_reset(&state);
		memcpy(params.wind, GALE, sizeof(params.wind));
		for(int i = 0; i < 180; i++) {
			g_pose = g_rest;
			character_spring_update(&g_male, &rig, &params, &matrix, 1.0f / 60.0f, &state, &g_pose);
		}
		memset(params.wind, 0, sizeof(params.wind));
		for(int i = 0; i < 240; i++) {
			g_pose = g_rest;
			character_spring_update(&g_male, &rig, &params, &matrix, 1.0f / 60.0f, &state, &g_pose);
		}
		CHECK(worst_angle(&g_male, &rig, &g_pose, &g_rest) < 0.12f,
		      "4 s after the wind drops the hair hangs within 7 degrees of its rest");
	}
}

int main(void) {
	char error[256] = "";

	printf("=== character spring ===\n");

	if(!skinned_model_load_glb(&g_male, CHARACTER_MODEL_MALE_PATH, error, (int)sizeof(error)) ||
	   !skinned_model_load_glb(&g_female, CHARACTER_MODEL_FEMALE_PATH, error, (int)sizeof(error)) ||
	   !skinned_model_load_glb(&g_kit, CHARACTER_MODEL_PATH, error, (int)sizeof(error))) {
		printf("  FAIL could not load a model: %s\n", error);
		return 1;
	}
	skinned_model_bind_pose(&g_male, &g_rest);
	g_params = hair_params();

	test_rig();
	test_rest();
	test_trail();
	test_bones();
	test_bodies();
	test_female();
	test_tail();
	test_wind();

	printf("\n%s (%d failures)\n", g_failures ? "FAILED" : "ALL PASSED", g_failures);

	return g_failures ? 1 : 0;
}
