// Spring bone colliders: spheres under the skin keep bones out of the body

#include "render/character_spring.h"
#include "render/character_tuning.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

// A vertex belongs to the collider bone that moves it at least this much
#define MIN_OWNER_WEIGHT 0.3f
// A vertex has a layer over it when one lies this far out along its normal (m)
#define COVER_MIN 0.003f
#define COVER_MAX 0.04f
// and within this much sideways, plus this share of how far out it is
#define COVER_SIDE 0.01f
#define COVER_SLOPE 0.5f
#define GRID_CELL 0.04f
#define GRID_SIZE 65536
// A push smaller than this (metres) is not worth a turn of the bone
#define MIN_PUSH 0.0003f
// A tip resting deep inside a sphere is kept out of this much of it (of 255)
#define MIN_REACH 8.0f

static int joint_of_node(const Skeleton* skeleton, int node) {
	for(int j = 0; j < skeleton->joint_count; j++)
		if(skeleton->joint_node[j] == node)
			return j;

	return -1;
}

// The collider bone a bone's skin goes with: itself, or above a J_Sec_ one
static int owner_node(const Skeleton* skeleton, const char* is_collider, int node) {
	const char* name = skeleton->node_names[node];
	int follows = strncmp(name, CHARACTER_COLLIDER_FOLLOW_PREFIX,
	                      strlen(CHARACTER_COLLIDER_FOLLOW_PREFIX)) == 0 ||
	              strstr(name, "_twist_") != NULL;

	for(int n = node; n >= 0; n = skeleton->parent[n]) {
		if(is_collider[n])
			return n;
		if(!follows)
			break;
	}

	return -1;
}

// The collider bone that moves a vertex most, or -1 (ears and tail: none)
static int vertex_owner(const SkinnedModel* model, const SkinnedMesh* mesh, int i,
                        const char* is_collider) {
	const Skeleton* skeleton = &model->skeleton;
	int owner[SKIN_INFLUENCES];
	float weight[SKIN_INFLUENCES];
	int best = -1;
	float most = MIN_OWNER_WEIGHT;

	for(int k = 0; k < SKIN_INFLUENCES; k++) {
		int moved_by = skeleton->joint_node[mesh->joints[i * SKIN_INFLUENCES + k]];

		owner[k] = -1;
		weight[k] = mesh->weights[i * SKIN_INFLUENCES + k];
		if(is_collider[moved_by] || strncmp(skeleton->node_names[moved_by],
		                                    CHARACTER_COLLIDER_SKIP_PREFIX,
		                                    strlen(CHARACTER_COLLIDER_SKIP_PREFIX)) != 0)
			owner[k] = owner_node(skeleton, is_collider, moved_by);
	}

	for(int k = 0; k < SKIN_INFLUENCES; k++) {
		float total = 0.0f;

		for(int m = 0; m < SKIN_INFLUENCES; m++)
			if(owner[m] >= 0 && owner[m] == owner[k])
				total += weight[m];
		if(owner[k] >= 0 && total > most) {
			most = total;
			best = owner[k];
		}
	}

	return best;
}

// Whether a sphere sits within `spacing` of `position` on the same bone
static int crowded(const character_spring_rig* rig, const float anchors[][3], int node,
                   const float position[3], float spacing) {
	for(int k = 0; k < rig->collider_count; k++) {
		float d[3] = { position[0] - anchors[k][0], position[1] - anchors[k][1],
		               position[2] - anchors[k][2] };

		if(rig->collider[k].node == node && d[0] * d[0] + d[1] * d[1] + d[2] * d[2] < spacing * spacing)
			return 1;
	}

	return 0;
}

// Lay a sphere under vertex `i` of `mesh`, unless one is close already
static void lay_sphere(const SkinnedModel* model, const SkinnedMesh* mesh, int i, int node,
                       float spacing, float radius, float margin, character_spring_rig* rig,
                       float anchors[][3]) {
	const float* position = &mesh->positions[i * 3];
	const float* normal = &mesh->normals[i * 3];
	float center[3];
	character_spring_collider* sphere;

	// A sphere on the tail only blocks: a braid is not drawn toward it
	int blocks_only = strncmp(model->skeleton.node_names[node], CHARACTER_COLLIDER_SKIP_PREFIX,
	                          strlen(CHARACTER_COLLIDER_SKIP_PREFIX)) == 0;
	if(rig->collider_count >= SPRING_MAX_COLLIDERS || crowded(rig, anchors, node, position, spacing))
		return;

	for(int a = 0; a < 3; a++)
		center[a] = position[a] - normal[a] * (radius - margin);
	sphere = &rig->collider[rig->collider_count];
	sphere->node = node;
	sphere->radius = radius;
	sphere->blocks_only = blocks_only;
	mat4_transform_point(&model->skeleton.inverse_bind[joint_of_node(&model->skeleton, node)],
	                     center, sphere->center);
	mat4_transform_direction(&model->skeleton.inverse_bind[joint_of_node(&model->skeleton, node)],
	                         normal, sphere->normal);
	memcpy(anchors[rig->collider_count], position, sizeof(float) * 3);
	rig->collider_count++;
}

static int bind_global_nodes(const Skeleton* skeleton, Mat4* global) {
	for(int i = 0; i < skeleton->node_count; i++) {
		int n = skeleton->order[i];
		const Transform* local = &skeleton->bind_local[n];
		Mat4 matrix = mat4_from_trs(local->translation, local->rotation, local->scale);

		global[n] = skeleton->parent[n] < 0 ? matrix
		                                    : mat4_multiply(global[skeleton->parent[n]], matrix);
	}

	return skeleton->node_count;
}

static int compare_by_node(const void* a, const void* b) {
	return ((const character_spring_collider*)a)->node - ((const character_spring_collider*)b)->node;
}

// Put each bone's colliders together and wrap each bone's in a sphere
static void cluster_colliders(character_spring_rig* rig) {
	qsort(rig->collider, (size_t)rig->collider_count, sizeof(rig->collider[0]), compare_by_node);

	rig->cluster_count = 0;
	for(int k = 0; k < rig->collider_count && rig->cluster_count < SPRING_MAX_CLUSTERS;) {
		character_spring_cluster* cluster = &rig->cluster[rig->cluster_count++];
		int end = k;

		while(end < rig->collider_count && rig->collider[end].node == rig->collider[k].node)
			end++;
		cluster->node = rig->collider[k].node;
		cluster->first = k;
		cluster->count = end - k;
		memset(cluster->center, 0, sizeof(cluster->center));
		for(int i = k; i < end; i++)
			for(int a = 0; a < 3; a++)
				cluster->center[a] += rig->collider[i].center[a] / (float)cluster->count;
		cluster->radius = 0.0f;
		for(int i = k; i < end; i++) {
			float d[3] = { rig->collider[i].center[0] - cluster->center[0],
			               rig->collider[i].center[1] - cluster->center[1],
			               rig->collider[i].center[2] - cluster->center[2] };
			float reach = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]) + rig->collider[i].radius;

			if(reach > cluster->radius)
				cluster->radius = reach;
		}
		k = end;
	}
}

// A strand rests as deep in a sphere as the model was made, and goes no deeper
static void measure_resting_tips(const SkinnedModel* model, character_spring_rig* rig) {
	Mat4 global[SKIN_MAX_NODES];
	static float centers[SPRING_MAX_COLLIDERS][3];
	static float normals[SPRING_MAX_COLLIDERS][3];
	float cluster_centers[SPRING_MAX_CLUSTERS][3];
	const float origin[3] = { 0.0f, 0.0f, 0.0f };

	bind_global_nodes(&model->skeleton, global);
	character_spring_collider_centers(rig, global, centers, normals, cluster_centers);
	memset(rig->collider_reach, 255, sizeof(rig->collider_reach));
	for(int j = 0; j < rig->count; j++) {
		float head[3], tip[3];

		mat4_transform_point(&global[rig->joint[j].node], origin, head);
		mat4_transform_point(&global[rig->joint[j].tip_node], origin, tip);
		for(int k = 0; k < rig->collider_count; k++) {
			float nearest = 1e9f;

			// The end and the middle of the bone, the two points that are kept out
			for(int half = 0; half < 2; half++) {
				float d[3];
				float distance;

				for(int i = 0; i < 3; i++)
					d[i] = (half ? tip[i] : (head[i] + tip[i]) * 0.5f) - centers[k][i];
				distance = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
				if(distance < nearest)
					nearest = distance;
			}
			if(nearest < rig->collider[k].radius + rig->joint[j].thickness)
				rig->collider_reach[j][k] = (unsigned char)fminf(255.0f, fmaxf(
				    (nearest - rig->joint[j].thickness) / rig->collider[k].radius * 255.0f,
				    MIN_REACH));
		}
	}
}

typedef struct {
	int count;
	float* position;       // 3 each
	float* normal;
	int* owner;            // the collider bone's node, or -1
	int* mesh;
	int* index;
	char* covered;         // 1 when another layer lies over it
} body_vertices;

static int cell_hash(int x, int y, int z) {
	return (int)(((unsigned)x * 73856093u ^ (unsigned)y * 19349663u ^ (unsigned)z * 83492791u) %
	             GRID_SIZE);
}

static void free_body(body_vertices* body) {
	free(body->position);
	free(body->normal);
	free(body->owner);
	free(body->mesh);
	free(body->index);
	free(body->covered);
	memset(body, 0, sizeof(*body));
}

// Whether a vertex lies just out along vertex `v`'s normal: a shirt covers skin
static int is_covered(const body_vertices* body, const int* head, const int* next, int v) {
	const float* p = &body->position[v * 3];
	const float* n = &body->normal[v * 3];
	int cell[3];

	for(int a = 0; a < 3; a++)
		cell[a] = (int)floorf(p[a] / GRID_CELL);
	for(int dx = -1; dx <= 1; dx++)
		for(int dy = -1; dy <= 1; dy++)
			for(int dz = -1; dz <= 1; dz++)
				for(int w = head[cell_hash(cell[0] + dx, cell[1] + dy, cell[2] + dz)]; w >= 0;
				    w = next[w]) {
					const float* q = &body->position[w * 3];
					float d[3] = { q[0] - p[0], q[1] - p[1], q[2] - p[2] };
					float out = d[0] * n[0] + d[1] * n[1] + d[2] * n[2];
					float side = sqrtf(fmaxf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2] - out * out,
					                         0.0f));

					if(out > COVER_MIN && out < COVER_MAX && side < COVER_SIDE + COVER_SLOPE * out)
						return 1;
				}

	return 0;
}

static int gather_body(const SkinnedModel* model, const char* is_collider, body_vertices* body) {
	int total = 0;
	int* head;
	int* next;

	memset(body, 0, sizeof(*body));
	for(int m = 0; m < model->mesh_count; m++)
		total += model->meshes[m].vertex_count;
	body->position = malloc((size_t)(total + 1) * 3 * sizeof(float));
	body->normal = malloc((size_t)(total + 1) * 3 * sizeof(float));
	body->owner = malloc((size_t)(total + 1) * sizeof(int));
	body->mesh = malloc((size_t)(total + 1) * sizeof(int));
	body->index = malloc((size_t)(total + 1) * sizeof(int));
	body->covered = calloc((size_t)(total + 1), 1);
	head = malloc(GRID_SIZE * sizeof(int));
	next = malloc((size_t)(total + 1) * sizeof(int));
	if(!body->position || !body->normal || !body->owner || !body->mesh || !body->index ||
	   !body->covered || !head || !next) {
		free_body(body);
		free(head);
		free(next);
		return 0;
	}

	for(int m = 0; m < model->mesh_count; m++) {
		const SkinnedMesh* mesh = &model->meshes[m];

		// The hair is what is being kept out
		if(strncmp(mesh->name, "Hair", 4) == 0 || !mesh->weights || !mesh->joints || !mesh->normals)
			continue;
		for(int i = 0; i < mesh->vertex_count; i++) {
			int owner = vertex_owner(model, mesh, i, is_collider);
			int v = body->count;

			if(owner < 0)
				continue;
			memcpy(&body->position[v * 3], &mesh->positions[i * 3], 3 * sizeof(float));
			memcpy(&body->normal[v * 3], &mesh->normals[i * 3], 3 * sizeof(float));
			body->owner[v] = owner;
			body->mesh[v] = m;
			body->index[v] = i;
			body->count++;
		}
	}

	for(int h = 0; h < GRID_SIZE; h++)
		head[h] = -1;
	for(int v = 0; v < body->count; v++) {
		int h = cell_hash((int)floorf(body->position[v * 3] / GRID_CELL),
		                  (int)floorf(body->position[v * 3 + 1] / GRID_CELL),
		                  (int)floorf(body->position[v * 3 + 2] / GRID_CELL));

		next[v] = head[h];
		head[h] = v;
	}
	for(int v = 0; v < body->count; v++)
		body->covered[v] = (char)is_covered(body, head, next, v);
	free(head);
	free(next);

	return 1;
}

int character_spring_fit_colliders(const SkinnedModel* model, const char* const* bones,
                                   int bone_count, float spacing, float radius, float margin,
                                   character_spring_rig* rig) {
	const Skeleton* skeleton = &model->skeleton;
	static float anchors[SPRING_MAX_COLLIDERS][3];
	char is_collider[SKIN_MAX_NODES] = { 0 };
	body_vertices body;

	rig->collider_count = 0;
	for(int b = 0; b < bone_count; b++)
		for(int n = 0; n < skeleton->node_count; n++)
			if(strcmp(skeleton->node_names[n], bones[b]) == 0 && joint_of_node(skeleton, n) >= 0)
				is_collider[n] = 1;

	if(gather_body(model, is_collider, &body)) {
		for(int v = 0; v < body.count; v++)
			if(!body.covered[v])
				lay_sphere(model, &model->meshes[body.mesh[v]], body.index[v], body.owner[v], spacing,
				           radius, margin, rig, anchors);
		free_body(&body);
	}
	cluster_colliders(rig);
	measure_resting_tips(model, rig);

	return rig->collider_count;
}

void character_spring_collider_centers(const character_spring_rig* rig, const Mat4* global,
                                       float centers[][3], float normals[][3],
                                       float cluster_centers[][3]) {
	for(int k = 0; k < rig->collider_count; k++) {
		mat4_transform_point(&global[rig->collider[k].node], rig->collider[k].center, centers[k]);
		mat4_transform_direction(&global[rig->collider[k].node], rig->collider[k].normal,
		                         normals[k]);
	}
	for(int c = 0; c < rig->cluster_count; c++)
		mat4_transform_point(&global[rig->cluster[c].node], rig->cluster[c].center,
		                     cluster_centers[c]);
}

int character_spring_nearby_colliders(const character_spring_rig* rig, const float centers[][3],
                                      const float cluster_centers[][3], const float point[3],
                                      float reach, int* out) {
	int count = 0;

	for(int c = 0; c < rig->cluster_count; c++) {
		const character_spring_cluster* cluster = &rig->cluster[c];
		float d[3] = { point[0] - cluster_centers[c][0], point[1] - cluster_centers[c][1],
		               point[2] - cluster_centers[c][2] };
		float range = reach + cluster->radius;

		if(d[0] * d[0] + d[1] * d[1] + d[2] * d[2] >= range * range)
			continue;
		for(int k = cluster->first; k < cluster->first + cluster->count; k++) {
			float e[3] = { point[0] - centers[k][0], point[1] - centers[k][1],
			               point[2] - centers[k][2] };
			float near = reach + rig->collider[k].radius;

			if(e[0] * e[0] + e[1] * e[1] + e[2] * e[2] < near * near)
				out[count++] = k;
		}
	}

	return count;
}

int character_spring_push_out(const character_spring_rig* rig, int joint,
                              const float centers[][3], const float normals[][3],
                              const int* nearby, int count, float extra, float point[3]) {
	int moved = 0;

	for(int n = 0; n < count; n++) {
		int k = nearby[n];
		float clear = rig->collider[k].blocks_only ? 0.0f : extra;
		float widest = rig->collider[k].radius + clear;
		float d[3] = { point[0] - centers[k][0], point[1] - centers[k][1],
		               point[2] - centers[k][2] };
		float squared = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
		float radius, along, sideways, out;

		// Most are too far to touch: say so before working out this strand's reach
		if(squared >= widest * widest)
			continue;
		radius = rig->collider[k].radius * (float)rig->collider_reach[joint][k] / 255.0f + clear;
		if(squared >= radius * radius)
			continue;

		along = d[0] * normals[k][0] + d[1] * normals[k][1] + d[2] * normals[k][2];
		if(along >= 0.0f) {
			// Out to the surface along the line from the middle: a strand slides smoothly
			out = radius - sqrtf(squared);
			if(out < MIN_PUSH)
				continue;
			for(int i = 0; i < 3; i++)
				point[i] += d[i] / fmaxf(sqrtf(squared), 1e-9f) * out;
		} else {
			// Past the middle that line goes deeper: leave the way the body faces instead
			sideways = squared - along * along;
			out = sqrtf(fmaxf(radius * radius - sideways, 0.0f)) - along;
			for(int i = 0; i < 3; i++)
				point[i] += normals[k][i] * out;
		}
		moved = 1;
	}

	return moved;
}
