// Spring bones stay out of the body: spheres under the skin, on the real models

#include "model/skinned_model.h"
#include "render/character_retarget.h"
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

#define PLAYER_SCALE 22.0f
#define FRAME 0.016666668f
// A point further than this (metres) from every vertex is not judged inside
#define NEAR_VERTEX 0.03f

static const char* const COLLIDER_BONES[] = CHARACTER_COLLIDER_BONES;
#define COLLIDER_BONE_COUNT ((int)(sizeof(COLLIDER_BONES) / sizeof(COLLIDER_BONES[0])))

static SkinnedModel g_kit, g_female, g_male;
static Pose g_rest, g_pose;
static SkinnedInstance g_skin;
static character_spring_params g_hair, g_tail;
static character_retarget g_retarget;

typedef struct {
	float x, z, yaw;
} body_state;

static Mat4 body_matrix(float x, float z, float yaw) {
	Mat4 move = mat4_translation(x, 0.0f, z);
	Mat4 turn = mat4_rotation_degrees(yaw, 0.0f, 1.0f, 0.0f);
	Mat4 grow = mat4_scaling(PLAYER_SCALE, PLAYER_SCALE, PLAYER_SCALE);

	return mat4_multiply(move, mat4_multiply(turn, grow));
}

static void node_globals(const SkinnedModel* model, const Pose* pose, Mat4* global) {
	const Skeleton* skeleton = &model->skeleton;

	for(int i = 0; i < skeleton->node_count; i++) {
		int n = skeleton->order[i];
		const Transform* local = &pose->local[n];
		Mat4 matrix = mat4_from_trs(local->translation, local->rotation, local->scale);
		int parent = skeleton->parent[n];

		global[n] = parent < 0 ? matrix : mat4_multiply(global[parent], matrix);
	}
}

// Whether a vertex is moved mostly by an optional part, the ears or the tail
static int optional_part(const SkinnedModel* model, int mesh, int vertex) {
	const SkinnedMesh* m = &model->meshes[mesh];
	int best = 0;
	float most = -1.0f;

	for(int k = 0; k < SKIN_INFLUENCES; k++)
		if(m->weights[vertex * SKIN_INFLUENCES + k] > most) {
			most = m->weights[vertex * SKIN_INFLUENCES + k];
			best = m->joints[vertex * SKIN_INFLUENCES + k];
		}

	return strncmp(model->skeleton.node_names[model->skeleton.joint_node[best]],
	               CHARACTER_COLLIDER_SKIP_PREFIX, strlen(CHARACTER_COLLIDER_SKIP_PREFIX)) == 0;
}

// Whether a vertex is moved mostly by an ear (the tail is something to lie on)
static int ear_part(const SkinnedModel* model, int mesh, int vertex) {
	const SkinnedMesh* m = &model->meshes[mesh];
	int best = 0;

	for(int k = 1; k < SKIN_INFLUENCES; k++)
		if(m->weights[vertex * SKIN_INFLUENCES + k] > m->weights[vertex * SKIN_INFLUENCES + best])
			best = k;

	return strstr(model->skeleton.node_names[model->skeleton.joint_node[
	              m->joints[vertex * SKIN_INFLUENCES + best]]], "Ear") != NULL;
}

// How far (metres) a point sits inside the skinned body or face, 0 when outside
static float sunk(const SkinnedModel* model, const float p[3]) {
	float nearest = 1e9f;
	float inside = 0.0f;

	for(int m = 0; m < model->mesh_count; m++) {
		const float* vertices = g_skin.meshes[m].vertices;

		if(strncmp(model->meshes[m].name, "Hair", 4) == 0 || !vertices)
			continue;
		for(int i = 0; i < g_skin.meshes[m].vertex_count; i++) {
			const float* v = vertices + i * SKIN_VERTEX_FLOATS;
			float d[3] = { p[0] - v[0], p[1] - v[1], p[2] - v[2] };
			float squared = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];

			if(squared < nearest && !optional_part(model, m, i)) {
				nearest = squared;
				inside = -(d[0] * v[3] + d[1] * v[4] + d[2] * v[5]);
			}
		}
	}

	// A vertex further off says nothing sure about which side a point is on
	return nearest < NEAR_VERTEX * NEAR_VERTEX && inside > 0.0f ? inside : 0.0f;
}

// The deepest any joint of the rig sits in the body in the current g_pose
static float deepest(const SkinnedModel* model, const character_spring_rig* rig) {
	static Mat4 global[SKIN_MAX_NODES];
	const float origin[3] = { 0.0f, 0.0f, 0.0f };
	float worst = 0.0f;

	node_globals(model, &g_pose, global);
	skinned_model_skin(model, &g_pose, &g_skin);
	for(int j = 0; j < rig->count; j++) {
		float a[3], b[3], p[3];

		mat4_transform_point(&global[rig->joint[j].node], origin, a);
		mat4_transform_point(&global[rig->joint[j].tip_node], origin, b);
		for(int k = 0; k < 2; k++) {
			float d;

			for(int i = 0; i < 3; i++)
				p[i] = a[i] + (b[i] - a[i]) * (k == 0 ? 0.5f : 1.0f);
			d = sunk(model, p);
			if(d > worst)
				worst = d;
		}
	}

	return worst;
}

// How far (metres) a point is from the nearest body vertex, ears and tail aside
static int compare_floats(const void* a, const void* b) {
	float x = *(const float*)a;
	float y = *(const float*)b;

	return (x > y) - (x < y);
}

static float distance_to_body(const SkinnedModel* model, const float p[3]) {
	float nearest = 1e9f;

	for(int m = 0; m < model->mesh_count; m++) {
		const float* vertices = g_skin.meshes[m].vertices;

		if(strncmp(model->meshes[m].name, "Hair", 4) == 0 || !vertices)
			continue;
		for(int i = 0; i < g_skin.meshes[m].vertex_count; i++) {
			const float* v = vertices + i * SKIN_VERTEX_FLOATS;
			float d[3] = { p[0] - v[0], p[1] - v[1], p[2] - v[2] };
			float squared = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];

			if(squared < nearest && !ear_part(model, m, i))
				nearest = squared;
		}
	}

	return sqrtf(nearest);
}

typedef enum { RUN_STOP, STRAFE, SPIN, BACK_UP, SCENARIO_COUNT } scenario;

// Drive a body through a scenario; the deepest the rig sinks over it, metres
static float drive(const SkinnedModel* model, const character_spring_rig* rig,
                   const character_spring_params* params, scenario kind) {
	static character_spring_state state;
	body_state body = { 0.0f, 0.0f, 0.0f };
	float worst = 0.0f;

	character_spring_reset(&state);
	for(int f = 0; f < 300; f++) {
		float t = (float)f * FRAME;
		Mat4 matrix;

		if(kind == RUN_STOP && t > 0.5f && t < 2.0f)
			body.z += 200.0f * FRAME;
		if(kind == STRAFE && t > 0.5f && t < 2.0f)
			body.x += 200.0f * FRAME;
		if(kind == BACK_UP && t > 0.5f && t < 1.5f)
			body.z -= 150.0f * FRAME;
		if(kind == SPIN)
			body.yaw = t < 0.5f ? 0.0f : fminf(180.0f, (t - 0.5f) * 360.0f);
		matrix = body_matrix(body.x, body.z, body.yaw);
		g_pose = g_rest;
		character_spring_update(model, rig, params, &matrix, FRAME, &state, &g_pose);
		if(f % 10 == 5) {
			float d = deepest(model, rig);

			if(d > worst)
				worst = d;
		}
	}

	return worst;
}

static void test_fit(void) {
	static character_spring_rig rig;

	printf("\nTEST 1: colliders are fitted to the bones named\n");
	character_spring_find(&g_female, CHARACTER_HAIR_PREFIX, &rig);
	CHECK(rig.collider_count == 0, "a new rig has none");
	CHECK(character_spring_fit_colliders(&g_female, COLLIDER_BONES, COLLIDER_BONE_COUNT,
	                                     CHARACTER_COLLIDER_SPACING, CHARACTER_COLLIDER_RADIUS, CHARACTER_COLLIDER_MARGIN,
	                                     &rig) == rig.collider_count,
	      "the count returned is the count kept");
	CHECK(rig.collider_count >= 100, "the body is covered by at least a hundred spheres");
	CHECK(rig.collider_count < SPRING_MAX_COLLIDERS, "and they fit in the rig");
	{
		int sane = 1;

		for(int k = 0; k < rig.collider_count; k++) {
			const character_spring_collider* c = &rig.collider[k];

			if(c->radius != CHARACTER_COLLIDER_RADIUS || fabsf(c->center[0]) > 1.0f ||
			   fabsf(c->center[1]) > 1.0f || fabsf(c->center[2]) > 1.0f)
				sane = 0;
		}
		CHECK(sane, "every sphere has the radius asked for and sits within a metre of its bone");
	}
	CHECK(character_spring_fit_colliders(&g_female, COLLIDER_BONES, 0, 0.035f, 0.03f, 0.0f, &rig) == 0,
	      "no bones, no colliders");
}

static void test_depth(const SkinnedModel* model, const char* name) {
	static character_spring_rig plain, guarded;
	static const char* const SCENARIO_NAMES[SCENARIO_COUNT] = { "run and stop", "strafe",
	                                                            "spin", "back up" };
	float rest_depth;
	float worst_free = 0.0f;
	float worst_guarded = 0.0f;

	printf("\nTEST 2: the %s model's hair stays out of the body\n", name);
	character_spring_find(model, CHARACTER_HAIR_PREFIX, &plain);
	character_spring_find(model, CHARACTER_HAIR_PREFIX, &guarded);
	character_spring_fit_colliders(model, COLLIDER_BONES, COLLIDER_BONE_COUNT,
	                               CHARACTER_COLLIDER_SPACING, CHARACTER_COLLIDER_RADIUS, CHARACTER_COLLIDER_MARGIN,
	                               &guarded);
	skinned_model_bind_pose(model, &g_rest);
	g_pose = g_rest;
	rest_depth = deepest(model, &guarded);
	printf("  (as authored the hair sinks %.1f mm)\n", rest_depth * 1000.0f);

	for(int s = 0; s < SCENARIO_COUNT; s++) {
		float free_depth = drive(model, &plain, &g_hair, (scenario)s);
		float depth = drive(model, &guarded, &g_hair, (scenario)s);
		char what[96];

		printf("  %-13s without colliders %.1f mm, with %.1f mm\n", SCENARIO_NAMES[s],
		       free_depth * 1000.0f, depth * 1000.0f);
		snprintf(what, sizeof(what), "%s: no deeper than as authored plus 8 mm", SCENARIO_NAMES[s]);
		CHECK(depth <= rest_depth + 0.008f, what);
		if(free_depth > worst_free)
			worst_free = free_depth;
		if(depth > worst_guarded)
			worst_guarded = depth;
	}
	CHECK(worst_guarded < worst_free * 0.6f, "the colliders cut the worst sink by at least 40%");
}

static float length3(const float v[3]) {
	return sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}

static void test_tail(void) {
	static character_spring_rig tail;
	float rest_depth;
	float worst = 0.0f;

	printf("\nTEST 3: the tail, swinging free, stays out of the body\n");
	character_spring_find(&g_female, CHARACTER_TAIL_PREFIX, &tail);
	skinned_model_bind_pose(&g_female, &g_rest);
	g_pose = g_rest;
	rest_depth = deepest(&g_female, &tail);
	for(int s = 0; s < SCENARIO_COUNT; s++) {
		float depth = drive(&g_female, &tail, &g_tail, (scenario)s);

		printf("  scenario %d: %.1f mm\n", s, depth * 1000.0f);
		if(depth > worst)
			worst = depth;
	}
	CHECK(worst <= rest_depth + 0.012f, "no deeper than as authored plus 12 mm");
}

static void test_calm(void) {
	static character_spring_rig rig;
	static character_spring_state state;
	Mat4 matrix = body_matrix(10.0f, 20.0f, 40.0f);
	float worst = 0.0f;

	printf("\nTEST 4: colliders do not push the hair about while the body stands still\n");
	character_spring_find(&g_female, CHARACTER_HAIR_PREFIX, &rig);
	character_spring_fit_colliders(&g_female, COLLIDER_BONES, COLLIDER_BONE_COUNT,
	                               CHARACTER_COLLIDER_SPACING, CHARACTER_COLLIDER_RADIUS, CHARACTER_COLLIDER_MARGIN, &rig);
	skinned_model_bind_pose(&g_female, &g_rest);
	character_spring_reset(&state);
	for(int i = 0; i < 300; i++) {
		g_pose = g_rest;
		character_spring_update(&g_female, &rig, &g_hair, &matrix, FRAME, &state, &g_pose);
	}
	for(int j = 0; j < rig.count; j++) {
		float angle = 0.0f;
		const float* a = g_pose.local[rig.joint[j].node].rotation;

		// A braid is meant to droop away from the pose it was made in
		if(rig.joint[j].chain >= CHARACTER_HAIR_DRAPE_LENGTH)
			continue;
		const float* b = g_rest.local[rig.joint[j].node].rotation;
		float dot = fabsf(a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]);

		angle = 2.0f * acosf(fminf(1.0f, dot));
		if(angle > worst)
			worst = angle;
	}
	CHECK(worst < 0.45f, "every hair bone is within 26 degrees of the pose after 5 s");
}

// The 10th percentile of a joint's skin distance from the body
static float skin_gap(const SkinnedModel* model, const character_spring_rig* rig, int joint) {
	static float gaps[4000];
	int hair = -1;
	int n = 0;
	const SkinnedMesh* mesh;
	const float* skinned;

	for(int m = 0; m < model->mesh_count; m++)
		if(strncmp(model->meshes[m].name, "Hair", 4) == 0)
			hair = m;
	if(hair < 0)
		return 0.0f;
	mesh = &model->meshes[hair];
	skinned = g_skin.meshes[hair].vertices;
	for(int v = 0; v < mesh->vertex_count && n < 4000; v += 3) {
		int best = 0;

		for(int k = 1; k < SKIN_INFLUENCES; k++)
			if(mesh->weights[v * SKIN_INFLUENCES + k] > mesh->weights[v * SKIN_INFLUENCES + best])
				best = k;
		if(model->skeleton.joint_node[mesh->joints[v * SKIN_INFLUENCES + best]] !=
		   rig->joint[joint].node)
			continue;
		gaps[n++] = distance_to_body(model, skinned + v * SKIN_VERTEX_FLOATS);
	}
	if(n < 8)
		return -1.0f;
	qsort(gaps, (size_t)n, sizeof(float), compare_floats);

	return gaps[n / 10];
}

// How close the braids lie to her: the mean over their bones of that tenth
static void braid_gaps(const SkinnedModel* model, const character_spring_rig* rig, float* mean,
                       float* worst, int* bones) {
	float sum = 0.0f;

	*worst = 0.0f;
	*bones = 0;
	skinned_model_skin(model, &g_pose, &g_skin);
	for(int j = 0; j < rig->count; j++) {
		float gap;

		if(rig->joint[j].chain < CHARACTER_HAIR_DRAPE_LENGTH)
			continue;
		gap = skin_gap(model, rig, j);
		if(gap < 0.0f)
			continue;
		sum += gap;
		(*bones)++;
		if(gap > *worst)
			*worst = gap;
	}
	*mean = *bones > 0 ? sum / (float)*bones : 0.0f;
}

// The kit's idle clip on the female model, as the game poses her standing
static void idle_pose(float seconds) {
	static Pose kit_pose;

	skinned_model_sample(&g_kit, skinned_model_find_clip(&g_kit, CHARACTER_CLIP_IDLE), seconds,
	                     &kit_pose);
	character_retarget_apply(&g_retarget, &g_kit, &g_female, &kit_pose, &g_pose);
}

static void test_braids(void) {
	static character_spring_rig rig;
	static character_spring_state state;
	Mat4 matrix = body_matrix(0.0f, 0.0f, 0.0f);
	float mean, worst;
	int bones;

	printf("\nTEST 6: the long braids rest against her back\n");
	character_spring_find(&g_female, CHARACTER_HAIR_PREFIX, &rig);
	character_spring_fit_colliders(&g_female, COLLIDER_BONES, COLLIDER_BONE_COUNT,
	                               CHARACTER_COLLIDER_SPACING, CHARACTER_COLLIDER_RADIUS,
	                               CHARACTER_COLLIDER_MARGIN, &rig);
	idle_pose(0.0f);
	braid_gaps(&g_female, &rig, &mean, &worst, &bones);
	printf("  (as authored, %d bones: closest tenth %.1f mm on average, %.1f mm at worst)\n",
	       bones, mean * 1000.0f, worst * 1000.0f);
	CHECK(bones == 8, "the two braids' eight lower bones have skin of their own");

	character_spring_reset(&state);
	for(int i = 0; i < 300; i++) {
		idle_pose((float)i * FRAME);
		character_spring_update(&g_female, &rig, &g_hair, &matrix, FRAME, &state, &g_pose);
	}
	braid_gaps(&g_female, &rig, &mean, &worst, &bones);
	printf("  (after 5 s standing: %.1f mm on average, %.1f mm at worst)\n", mean * 1000.0f,
	       worst * 1000.0f);
	CHECK(mean < 0.032f, "standing, the closest tenth of each braid is within 3.2 cm of her");
	CHECK(worst < 0.045f, "and on no bone is it more than 4.5 cm off");

	// Resting, they hold still
	{
		float before[3], after[3], moved = 0.0f;
		const float origin[3] = { 0.0f, 0.0f, 0.0f };
		static Mat4 global[SKIN_MAX_NODES];
		int tip = 0;

		for(int j = 0; j < rig.count; j++)
			if(rig.joint[j].chain >= CHARACTER_HAIR_DRAPE_LENGTH)
				tip = rig.joint[j].tip_node;
		node_globals(&g_female, &g_pose, global);
		mat4_transform_point(&global[tip], origin, before);
		for(int i = 300; i < 330; i++) {
			idle_pose(5.0f + (float)(i - 300) * FRAME);
			character_spring_update(&g_female, &rig, &g_hair, &matrix, FRAME, &state, &g_pose);
		}
		node_globals(&g_female, &g_pose, global);
		mat4_transform_point(&global[tip], origin, after);
		for(int i = 0; i < 3; i++)
			moved += fabsf(after[i] - before[i]);
		CHECK(moved < 0.03f, "and a braid's end drifts under 3 cm in half a second of standing");
	}
}

// How far a hair vertex is inside her shirt or body; 0 if out, or head or neck
static float shirt_sink(const SkinnedModel* model, const float p[3]) {
	float nearest = 1e9f;
	float inside = 0.0f;
	int owner = -1;

	for(int m = 0; m < model->mesh_count; m++) {
		const SkinnedMesh* mesh = &model->meshes[m];
		const float* vertices = g_skin.meshes[m].vertices;

		if(strncmp(mesh->name, "Hair", 4) == 0 || !vertices)
			continue;
		for(int i = 0; i < g_skin.meshes[m].vertex_count; i++) {
			const float* v = vertices + i * SKIN_VERTEX_FLOATS;
			float d[3] = { p[0] - v[0], p[1] - v[1], p[2] - v[2] };
			float squared = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];

			if(squared < nearest && !ear_part(model, m, i)) {
				int best = 0;

				nearest = squared;
				inside = -(d[0] * v[3] + d[1] * v[4] + d[2] * v[5]);
				for(int k = 1; k < SKIN_INFLUENCES; k++)
					if(mesh->weights[i * SKIN_INFLUENCES + k] >
					   mesh->weights[i * SKIN_INFLUENCES + best])
						best = k;
				owner = model->skeleton.joint_node[mesh->joints[i * SKIN_INFLUENCES + best]];
			}
		}
	}
	if(owner >= 0 && (strcmp(model->skeleton.node_names[owner], "Head") == 0 ||
	                  strcmp(model->skeleton.node_names[owner], "neck_01") == 0))
		return 0.0f;

	return nearest < NEAR_VERTEX * NEAR_VERTEX && inside > 0.0f ? inside : 0.0f;
}

#define VISIBLE_STRIDE 16
#define VISIBLE_MAX 2048

// How much deeper than authored the sampled hair vertices sit: largest and p99
static void visible_sink(const SkinnedModel* model, int hair, const float* authored, float* worst,
                         float* p99, float* over) {
	static float excess[VISIBLE_MAX];
	const float* skinned;
	int n = 0;

	skinned_model_skin(model, &g_pose, &g_skin);
	skinned = g_skin.meshes[hair].vertices;
	int deeper_than_5 = 0;

	*worst = 0.0f;
	for(int v = 0; v < g_skin.meshes[hair].vertex_count && n < VISIBLE_MAX; v += VISIBLE_STRIDE) {
		float deeper = shirt_sink(model, skinned + v * SKIN_VERTEX_FLOATS) - authored[n];

		excess[n] = deeper > 0.0f ? deeper : 0.0f;
		if(excess[n] > *worst)
			*worst = excess[n];
		if(excess[n] > 0.005f)
			deeper_than_5++;
		n++;
	}
	qsort(excess, (size_t)n, sizeof(float), compare_floats);
	*p99 = excess[(int)((float)n * 0.99f)];
	*over = (float)deeper_than_5 / (float)n;
}

// The hair she wears, on some of the kit's animations, against as authored
static void test_visible(void) {
	static character_spring_rig rig;
	static character_spring_state state;
	static float authored[VISIBLE_MAX];
	static const char* const CLIPS[] = { "Idle_Loop", "Walk_Loop", "Sword_Idle", "Sitting_Enter",
	                                     "Dance_Loop", "Pistol_Aim_Neutral", "Sprint_Loop" };
	Mat4 matrix = body_matrix(0.0f, 0.0f, 0.0f);
	int hair = -1;
	int n = 0;

	printf("\nTEST 7: the hair she wears stays out of her shirt in motion\n");
	for(int m = 0; m < g_female.mesh_count; m++)
		if(strncmp(g_female.meshes[m].name, "Hair", 4) == 0)
			hair = m;
	character_spring_find(&g_female, CHARACTER_HAIR_PREFIX, &rig);
	character_spring_fit_colliders(&g_female, COLLIDER_BONES, COLLIDER_BONE_COUNT,
	                               CHARACTER_COLLIDER_SPACING, CHARACTER_COLLIDER_RADIUS,
	                               CHARACTER_COLLIDER_MARGIN, &rig);

	// How deep each sampled vertex sits as authored
	g_pose = g_rest;
	skinned_model_skin(&g_female, &g_pose, &g_skin);
	for(int v = 0; v < g_skin.meshes[hair].vertex_count && n < VISIBLE_MAX; v += VISIBLE_STRIDE)
		authored[n++] = shirt_sink(&g_female, g_skin.meshes[hair].vertices + v * SKIN_VERTEX_FLOATS);

	for(int c = 0; c < (int)(sizeof(CLIPS) / sizeof(CLIPS[0])); c++) {
		int clip = skinned_model_find_clip(&g_kit, CLIPS[c]);
		float duration = g_kit.clips[clip].duration;
		float worst = 0.0f;
		float p99 = 0.0f;
		float over = 0.0f;
		float z = 0.0f;
		int sprint = strcmp(CLIPS[c], "Sprint_Loop") == 0;
		char what[128];

		character_spring_reset(&state);
		for(int f = 0; f < 240; f++) {
			static Pose kit_pose;
			float w, p, o;

			skinned_model_sample(&g_kit, clip, fmodf((float)f * FRAME, duration), &kit_pose);
			character_retarget_apply(&g_retarget, &g_kit, &g_female, &kit_pose, &g_pose);
			z += (sprint ? 180.0f : 0.0f) * FRAME;
			matrix = body_matrix(0.0f, z, 0.0f);
			character_spring_update(&g_female, &rig, &g_hair, &matrix, FRAME, &state, &g_pose);
			if(f % 12 != 11)
				continue;
			visible_sink(&g_female, hair, authored, &w, &p, &o);
			if(w > worst)
				worst = w;
			if(p > p99)
				p99 = p;
			if(o > over)
				over = o;
		}
		printf("  %-20s deepest %.1f mm, 99th percentile %.1f mm, %.1f%% over 5 mm\n", CLIPS[c],
		       worst * 1000.0f, p99 * 1000.0f, over * 100.0f);
		// A few shoulder tufts can dip into an arm or chest; so can a sprint's root
		snprintf(what, sizeof(what), "%s: the 99th percentile is within %d mm of as authored",
		         CLIPS[c], sprint ? 12 : 5);
		CHECK(p99 < (sprint ? 0.012f : 0.005f), what);
		snprintf(what, sizeof(what), "%s: under %d%% of the hair is more than 5 mm deeper", CLIPS[c],
		         sprint ? 6 : 3);
		CHECK(over < (sprint ? 0.06f : 0.03f), what);
	}
}

typedef struct {
	float fold_min;   // the braid's end-to-end length over its length: 1 is straight
	float jerk_rms;   // how much its ends accelerate each frame, metres
	int crowded;      // frames in which two of a braid's joints sit nearly on one another
	int frames;
} flow_stats;

// Drive her through a clip, speeding up and slowing, and watch the braids
static void flow_run(const character_spring_rig* rig, const char* name, float speed,
                     flow_stats* out) {
	static character_spring_state state;
	static Pose kit_pose;
	static Mat4 global[SKIN_MAX_NODES];
	float tip[SPRING_MAX_JOINTS][3], head[SPRING_MAX_JOINTS][3];
	float before[SPRING_MAX_JOINTS][3], before2[SPRING_MAX_JOINTS][3];
	const float origin[3] = { 0.0f, 0.0f, 0.0f };
	int clip = skinned_model_find_clip(&g_kit, name);
	float duration = g_kit.clips[clip].duration;
	double sum = 0.0;
	int samples = 0;
	float z = 0.0f;

	memset(out, 0, sizeof(*out));
	out->fold_min = 9.0f;
	character_spring_reset(&state);
	for(int f = 0; f < 360; f++) {
		float t = (float)f * FRAME;
		float now = speed * (t < 1.0f ? t : t < 4.0f ? 1.0f : fmaxf(0.0f, 1.0f - (t - 4.0f)));
		Mat4 matrix;

		z += now * FRAME;
		skinned_model_sample(&g_kit, clip, fmodf(t, duration), &kit_pose);
		character_retarget_apply(&g_retarget, &g_kit, &g_female, &kit_pose, &g_pose);
		matrix = mat4_multiply(mat4_translation(0.0f, 0.0f, z),
		                       mat4_scaling(PLAYER_SCALE, PLAYER_SCALE, PLAYER_SCALE));
		character_spring_update(&g_female, rig, &g_hair, &matrix, FRAME, &state, &g_pose);
		node_globals(&g_female, &g_pose, global);
		for(int j = 0; j < rig->count; j++) {
			mat4_transform_point(&global[rig->joint[j].tip_node], origin, tip[j]);
			mat4_transform_point(&global[rig->joint[j].node], origin, head[j]);
		}
		if(f > 32) {
			int crowded = 0;

			for(int j = 0; j < rig->count; j++) {
				int chain[8], n = 0, at = j;
				float total = 0.0f, ends[3], ratio;

				if(rig->joint[j].chain < CHARACTER_HAIR_DRAPE_LENGTH)
					continue;
				if(rig->joint[j].parent < 0) {
					chain[n++] = at;
					for(int next = 0; next >= 0 && n < 8;) {
						next = -1;
						for(int q = 0; q < rig->count; q++)
							if(rig->joint[q].parent == at)
								next = q;
						if(next >= 0) {
							chain[n++] = next;
							at = next;
						}
					}
					for(int q = 0; q < n; q++) {
						for(int i = 0; i < 3; i++)
							ends[i] = tip[chain[q]][i] - head[chain[q]][i];
						total += length3(ends);
					}
					for(int i = 0; i < 3; i++)
						ends[i] = tip[chain[n - 1]][i] - head[chain[0]][i];
					ratio = length3(ends) / total;
					if(ratio < out->fold_min)
						out->fold_min = ratio;
					for(int a = 0; a < n; a++)
						for(int b = a + 2; b < n; b++) {
							float d[3] = { tip[chain[a]][0] - tip[chain[b]][0],
							               tip[chain[a]][1] - tip[chain[b]][1],
							               tip[chain[a]][2] - tip[chain[b]][2] };

							if(length3(d) < 0.03f)
								crowded = 1;
						}
				}
				if(f > 34) {
					float a2[3] = { tip[j][0] - 2.0f * before[j][0] + before2[j][0],
					                tip[j][1] - 2.0f * before[j][1] + before2[j][1],
					                tip[j][2] - 2.0f * before[j][2] + before2[j][2] };

					sum += a2[0] * a2[0] + a2[1] * a2[1] + a2[2] * a2[2];
					samples++;
				}
			}
			out->crowded += crowded;
			out->frames++;
		}
		memcpy(before2, before, sizeof(before));
		memcpy(before, tip, sizeof(before));
	}
	out->jerk_rms = samples > 0 ? sqrtf((float)(sum / samples)) : 0.0f;
}

// The braids flow like a rope: no folding up, bunching or jerking about
static void test_flow(void) {
	static character_spring_rig free_rig, guarded;
	static const char* const CLIPS[] = { "Idle_Loop", "Walk_Loop", "Jog_Fwd_Loop", "Sprint_Loop" };
	static const float SPEED[] = { 0.0f, 21.0f, 117.0f, 180.0f };
	static const float FOLD[] = { 0.9f, 0.85f, 0.5f, 0.5f };

	printf("\nTEST 8: the braids flow cleanly, as they do with no body in the way\n");
	character_spring_find(&g_female, CHARACTER_HAIR_PREFIX, &free_rig);
	character_spring_find(&g_female, CHARACTER_HAIR_PREFIX, &guarded);
	character_spring_fit_colliders(&g_female, COLLIDER_BONES, COLLIDER_BONE_COUNT,
	                               CHARACTER_COLLIDER_SPACING, CHARACTER_COLLIDER_RADIUS,
	                               CHARACTER_COLLIDER_MARGIN, &guarded);
	for(int c = 0; c < 4; c++) {
		flow_stats open, blocked;
		char what[128];

		flow_run(&free_rig, CLIPS[c], SPEED[c], &open);
		flow_run(&guarded, CLIPS[c], SPEED[c], &blocked);
		printf("  %-14s fold %.2f (free %.2f), jerk %.5f (free %.5f), crowded %d of %d frames\n",
		       CLIPS[c], blocked.fold_min, open.fold_min, blocked.jerk_rms, open.jerk_rms,
		       blocked.crowded, blocked.frames);
		snprintf(what, sizeof(what), "%s: the braid never folds below %.2f of its length", CLIPS[c],
		         FOLD[c]);
		CHECK(blocked.fold_min >= FOLD[c], what);
		snprintf(what, sizeof(what), "%s: no two of a braid's joints bunch together", CLIPS[c]);
		CHECK(blocked.crowded == 0, what);
		snprintf(what, sizeof(what), "%s: it is no more than 3 times as jerky as with no body", CLIPS[c]);
		CHECK(blocked.jerk_rms <= open.jerk_rms * 3.0f + 0.0005f, what);
	}
}

static void test_stable(void) {
	static character_spring_rig rig;
	static character_spring_state state;
	static const float DT[3] = { 1.0f / 30.0f, 1.0f / 144.0f, 0.1f };

	printf("\nTEST 5: it stays stable with colliders at any frame rate\n");
	character_spring_find(&g_female, CHARACTER_HAIR_PREFIX, &rig);
	character_spring_fit_colliders(&g_female, COLLIDER_BONES, COLLIDER_BONE_COUNT,
	                               CHARACTER_COLLIDER_SPACING, CHARACTER_COLLIDER_RADIUS,
	                               CHARACTER_COLLIDER_MARGIN, &rig);
	for(int k = 0; k < 3; k++) {
		int finite = 1;
		float x = 0.0f;

		character_spring_reset(&state);
		for(float t = 0.0f; t < 3.0f; t += DT[k]) {
			Mat4 matrix;

			x += (t > 0.5f && t < 1.5f ? 220.0f : 0.0f) * DT[k];
			matrix = body_matrix(x, 0.0f, t * 200.0f);
			g_pose = g_rest;
			character_spring_update(&g_female, &rig, &g_hair, &matrix, DT[k], &state, &g_pose);
			for(int n = 0; n < g_female.skeleton.node_count; n++)
				for(int c = 0; c < 4; c++)
					if(!isfinite(g_pose.local[n].rotation[c]))
						finite = 0;
		}
		CHECK(finite, "no NaN");
	}
}

int main(void) {
	char error[256] = "";

	printf("=== character spring colliders ===\n");

	if(!skinned_model_load_glb(&g_female, CHARACTER_MODEL_FEMALE_PATH, error, (int)sizeof(error)) ||
	   !skinned_model_load_glb(&g_male, CHARACTER_MODEL_MALE_PATH, error, (int)sizeof(error))) {
		printf("  FAIL could not load a model: %s\n", error);
		return 1;
	}
	g_hair = (character_spring_params){ .stiffness = CHARACTER_HAIR_STIFFNESS,
	                                    .drag = CHARACTER_HAIR_DRAG,
	                                    .gravity = CHARACTER_HAIR_GRAVITY,
	                                    .drape_length = CHARACTER_HAIR_DRAPE_LENGTH,
	                                    .drape_stiffness = CHARACTER_HAIR_DRAPE_STIFFNESS,
	                                    .drape_gravity = CHARACTER_HAIR_DRAPE_GRAVITY,
	                                    .drape_press = CHARACTER_HAIR_DRAPE_PRESS,
	                                    .drape_reach = CHARACTER_HAIR_DRAPE_REACH };
	g_tail = (character_spring_params){ .stiffness = CHARACTER_TAIL_STIFFNESS,
	                                    .drag = CHARACTER_TAIL_DRAG,
	                                    .gravity = CHARACTER_TAIL_GRAVITY };
	if(!skinned_model_load_glb(&g_kit, CHARACTER_MODEL_PATH, error, (int)sizeof(error)) ||
	   character_retarget_build(&g_kit, &g_female, &g_retarget) == 0) {
		printf("  FAIL could not set up the kit's clips: %s\n", error);
		return 1;
	}
	skinned_model_bind_pose(&g_female, &g_rest);
	skinned_instance_init(&g_skin, &g_female);

	test_fit();
	test_depth(&g_female, "female");
	test_tail();
	test_calm();
	test_stable();
	test_braids();
	test_visible();
	test_flow();
	skinned_instance_free(&g_skin);

	skinned_instance_init(&g_skin, &g_male);
	test_depth(&g_male, "male");
	skinned_instance_free(&g_skin);

	printf("\n%s (%d failures)\n", g_failures ? "FAILED" : "ALL PASSED", g_failures);

	return g_failures ? 1 : 0;
}
