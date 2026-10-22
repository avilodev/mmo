#include "model/skinned_model.h"
#include "model/glb.h"
#include "utils/stb_image.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Reading a rigged character out of a binary glTF. The container work - bounds
   checking, accessor spans - is render/glb.h; what is here is the meaning laid
   over it: which nodes are bones, which vertices they move, and what the clips
   do to them over time.

   The rule this draws its lines by is the one the static reader used: refuse
   anything that would put the geometry in the wrong place, ignore anything
   that merely costs fidelity. A node carrying a baked matrix is refused,
   because reading it wrong bends the skeleton. A second UV set and every map
   but base colour are ignored, because a rig without them still stands in the
   right place and there is no shader here to sample them. */

/* ---- small readers ------------------------------------------------------ */

/* Reads a fixed-length float array member. Absent leaves `out` alone, which is
   how a glTF node says "identity"; present but unreadable is an error, since
   defaulting a rotation the file did state would bend the rig silently. */
static int read_float_array(JsonSlice object, const char* key, float* out, int count) {
    JsonSlice array, value;
    if (!json_member(object, key, &array)) return 1;
    for (int i = 0; i < count; ++i)
        if (!json_array_at(array, i, &value) || !json_float(value, &out[i])) return 0;
    return 1;
}

static void read_name(JsonSlice object, char* out, size_t capacity) {
    JsonSlice value;
    out[0] = 0;
    if (json_member(object, "name", &value)) json_string(value, out, capacity);
}

static int read_int_member(JsonSlice object, const char* key, int* out) {
    JsonSlice value;
    return json_member(object, key, &value) && json_int(value, out);
}

/* Indexes a nested array - an animation's channels, a mesh's primitives - into
   a freshly allocated table. Returns NULL and sets *count to 0 when absent. */
static JsonSlice* index_member_array(JsonSlice object, const char* key, int* count) {
    JsonSlice array;
    *count = 0;
    if (!json_member(object, key, &array)) return NULL;
    int n = json_array_count(array);
    if (n <= 0) return NULL;
    JsonSlice* table = malloc(sizeof *table * (size_t)n);
    if (!table) return NULL;
    if (json_array_index(array, table, n) != n) {
        free(table);
        return NULL;
    }
    *count = n;
    return table;
}

/* ---- nodes -------------------------------------------------------------- */

/* Orders nodes so a parent always precedes its children, which is what lets
   the pose walk compute global transforms in one forward pass. Any node left
   unplaced is in a parent cycle, which would loop that walk forever. */
static int order_hierarchy(Skeleton* skeleton, char* error, int error_size) {
    char placed[SKIN_MAX_NODES];
    memset(placed, 0, sizeof placed);
    int count = 0;
    for (int pass = 0; pass < skeleton->node_count && count < skeleton->node_count; ++pass) {
        int added = 0;
        for (int i = 0; i < skeleton->node_count; ++i) {
            if (placed[i]) continue;
            int parent = skeleton->parent[i];
            if (parent >= 0 && !placed[parent]) continue;
            skeleton->order[count++] = i;
            placed[i] = 1;
            added = 1;
        }
        if (!added) break;
    }
    if (count != skeleton->node_count)
        return glb_fail(error, error_size, "node hierarchy has a cycle");
    return 1;
}

static int parse_nodes(const Glb* glb, Skeleton* skeleton, char* error, int error_size) {
    JsonSlice nodes;
    if (!json_member(glb->root, "nodes", &nodes))
        return glb_fail(error, error_size, "glb has no nodes");
    int count = json_array_count(nodes);
    if (count <= 0) return glb_fail(error, error_size, "glb has no nodes");
    if (count > SKIN_MAX_NODES)
        return glb_fail(error, error_size, "glb has %d nodes; the limit is %d",
                        count, SKIN_MAX_NODES);

    JsonSlice* table = malloc(sizeof *table * (size_t)count);
    if (!table) return glb_fail(error, error_size, "out of memory for %d nodes", count);
    if (json_array_index(nodes, table, count) != count) {
        free(table);
        return glb_fail(error, error_size, "the nodes array is malformed");
    }

    skeleton->node_count = count;
    for (int i = 0; i < count; ++i) skeleton->parent[i] = -1;

    int ok = 1;
    for (int i = 0; i < count && ok; ++i) {
        JsonSlice node = table[i], value;
        Transform* local = &skeleton->bind_local[i];
        local->translation[0] = local->translation[1] = local->translation[2] = 0;
        local->rotation[0] = local->rotation[1] = local->rotation[2] = 0;
        local->rotation[3] = 1;
        local->scale[0] = local->scale[1] = local->scale[2] = 1;

        read_name(node, skeleton->node_names[i], SKIN_NAME_MAX);

        /* A matrix would have to be decomposed back into the parts animation
           interpolates, and a decomposition that guessed wrong about shear
           would bend the rig. Exporters that write one also offer TRS. */
        if (json_member(node, "matrix", &value)) {
            ok = glb_fail(error, error_size,
                          "node %d carries a baked matrix; this reader needs "
                          "translation/rotation/scale", i);
            break;
        }
        if (!read_float_array(node, "translation", local->translation, 3) ||
            !read_float_array(node, "rotation", local->rotation, 4) ||
            !read_float_array(node, "scale", local->scale, 3)) {
            ok = glb_fail(error, error_size, "node %d has a malformed transform", i);
            break;
        }

        JsonSlice children;
        if (json_member(node, "children", &children)) {
            int child_count = json_array_count(children);
            for (int c = 0; c < child_count; ++c) {
                int child = 0;
                if (!json_array_at(children, c, &value) || !json_int(value, &child) ||
                    child < 0 || child >= count) {
                    ok = glb_fail(error, error_size, "node %d has an unreadable child", i);
                    break;
                }
                if (skeleton->parent[child] != -1) {
                    ok = glb_fail(error, error_size, "node %d has two parents", child);
                    break;
                }
                skeleton->parent[child] = i;
            }
        }
    }
    free(table);
    return ok && order_hierarchy(skeleton, error, error_size);
}

/* ---- skin --------------------------------------------------------------- */

static int parse_skin(const Glb* glb, Skeleton* skeleton, char* error, int error_size) {
    JsonSlice skins, skin, joints, value;
    if (!json_member(glb->root, "skins", &skins) || !json_array_at(skins, 0, &skin))
        return glb_fail(error, error_size, "glb has no skin, so it holds no rigged mesh");
    if (!json_member(skin, "joints", &joints))
        return glb_fail(error, error_size, "skin has no joints");

    int count = json_array_count(joints);
    if (count <= 0) return glb_fail(error, error_size, "skin has no joints");
    if (count > SKIN_MAX_JOINTS)
        return glb_fail(error, error_size, "skin has %d joints; the limit is %d",
                        count, SKIN_MAX_JOINTS);
    for (int i = 0; i < count; ++i) {
        int node = 0;
        if (!json_array_at(joints, i, &value) || !json_int(value, &node) ||
            node < 0 || node >= skeleton->node_count)
            return glb_fail(error, error_size, "joint %d names a node that is not there", i);
        skeleton->joint_node[i] = node;
    }
    skeleton->joint_count = count;

    /* Without inverse bind matrices every vertex would be transformed as if
       the bone were at the origin, which turns the mesh inside out rather than
       merely looking wrong, so an absent accessor is refused. */
    int accessor_index = 0;
    if (!read_int_member(skin, "inverseBindMatrices", &accessor_index))
        return glb_fail(error, error_size, "skin has no inverseBindMatrices");
    GlbAccessor accessor;
    if (!glb_accessor(glb, accessor_index, "MAT4", 16, &accessor, error, error_size))
        return 0;
    if (accessor.count != count)
        return glb_fail(error, error_size, "skin has %d joints but %d bind matrices",
                        count, accessor.count);
    return glb_read_floats(&accessor, &skeleton->inverse_bind[0].m[0], error, error_size);
}

/* ---- meshes ------------------------------------------------------------- */

// How a material takes a tint, when a body is drawn with one: the tint times the
// result, or 0 for its own colour. "M_Joints" is the darker shade of the tint;
// any other name is left as authored. No material at all takes the plain tint.
static float material_shade(const char* name) {
	if(name[0] == '\0')
		return 1.0f;
	if(strncmp(name, "M_Joints", 8) == 0)
		return 0.45f;
	if(strncmp(name, "M_Main", 6) == 0)
		return 1.0f;
	return 0.0f;
}

static void read_material(const Glb* glb, JsonSlice primitive, SkinnedMesh* mesh) {
    JsonSlice materials, material, pbr, factor, value;
    int index = 0;
    mesh->base_color[0] = mesh->base_color[1] = mesh->base_color[2] = 1.0f;
	mesh->material[0] = '\0';
	mesh->tint_shade = material_shade(mesh->material);
    if (!read_int_member(primitive, "material", &index)) return;
    if (!json_member(glb->root, "materials", &materials) ||
        !json_array_at(materials, index, &material)) return;
	read_name(material, mesh->material, sizeof(mesh->material));
	mesh->tint_shade = material_shade(mesh->material);
    /* json.c has no boolean reader, so the literal is matched as raw text. */
    if (json_member(material, "doubleSided", &value))
        mesh->double_sided = value.length == 4 && memcmp(value.begin, "true", 4) == 0;
    if (!json_member(material, "pbrMetallicRoughness", &pbr)) return;
    if (json_member(pbr, "baseColorFactor", &factor))
        for (int i = 0; i < 3; ++i)
            if (json_array_at(factor, i, &value)) json_float(value, &mesh->base_color[i]);

    /* Decoded here, uploaded later: the loader owns no GL context. A map that
       will not decode leaves the mesh flat rather than failing the load, which
       is the same trade the static reader makes. */
    int texture = 0;
    if (json_member(pbr, "baseColorTexture", &value) &&
        read_int_member(value, "index", &texture))
        glb_decode_image(glb, texture, &mesh->image,
                         &mesh->image_width, &mesh->image_height);
}

/* Weights that do not sum to one brighten or shrink a vertex's displacement,
   which reads as a limb that swells as it bends. Exporters are usually close;
   normalizing costs one divide and removes the question. */
static void normalize_weights(float* weights, int vertex_count) {
    for (int i = 0; i < vertex_count; ++i) {
        float* w = weights + (size_t)i * SKIN_INFLUENCES;
        float sum = w[0] + w[1] + w[2] + w[3];
        if (sum > 1e-6f)
            for (int c = 0; c < SKIN_INFLUENCES; ++c) w[c] /= sum;
        else
            w[0] = 1, w[1] = w[2] = w[3] = 0;
    }
}

static int parse_primitive(const Glb* glb, JsonSlice primitive, int joint_count,
                           SkinnedMesh* mesh, char* error, int error_size) {
    JsonSlice attributes, value;
    GlbAccessor position, normal, joints, weights, indices, uvs;
    int index = 0, mode = 4;
    unsigned int* wide = NULL;

    if (json_member(primitive, "mode", &value) && json_int(value, &mode) && mode != 4)
        return glb_fail(error, error_size, "primitive mode %d is not a triangle list", mode);
    if (!json_member(primitive, "attributes", &attributes))
        return glb_fail(error, error_size, "primitive has no attributes");

    if (!read_int_member(attributes, "POSITION", &index) ||
        !glb_accessor(glb, index, "VEC3", 3, &position, error, error_size))
        return glb_fail(error, error_size, "primitive has no readable POSITION");
    if (!read_int_member(attributes, "NORMAL", &index) ||
        !glb_accessor(glb, index, "VEC3", 3, &normal, error, error_size))
        return glb_fail(error, error_size, "primitive has no NORMAL, which "
                                           "fixed-function lighting needs");
    if (!read_int_member(attributes, "JOINTS_0", &index) ||
        !glb_accessor(glb, index, "VEC4", 4, &joints, error, error_size))
        return glb_fail(error, error_size, "primitive has no JOINTS_0, so it is not skinned");
    if (!read_int_member(attributes, "WEIGHTS_0", &index) ||
        !glb_accessor(glb, index, "VEC4", 4, &weights, error, error_size))
        return glb_fail(error, error_size, "primitive has no WEIGHTS_0, so it is not skinned");
    if (!read_int_member(primitive, "indices", &index) ||
        !glb_accessor(glb, index, "SCALAR", 1, &indices, error, error_size))
        return glb_fail(error, error_size, "primitive is not indexed");

    /* A mesh with no map does not need UVs, and one that has them but no
       texture simply ignores them. Neither is an error, so this is the one
       attribute read on a maybe. */
    int has_uvs = read_int_member(attributes, "TEXCOORD_0", &index) &&
                  glb_accessor(glb, index, "VEC2", 2, &uvs, error, error_size);

    if (position.count != normal.count || position.count != joints.count ||
        position.count != weights.count || (has_uvs && position.count != uvs.count))
        return glb_fail(error, error_size, "primitive's attributes disagree on vertex count");
    if (indices.count % 3 != 0)
        return glb_fail(error, error_size, "index count %d is not a whole number of triangles",
                        indices.count);

    int vertex_count = position.count;
    mesh->vertex_count = vertex_count;
    mesh->index_count = indices.count;
    mesh->positions = malloc(sizeof(float) * 3 * (size_t)vertex_count);
    mesh->normals = malloc(sizeof(float) * 3 * (size_t)vertex_count);
    mesh->weights = malloc(sizeof(float) * SKIN_INFLUENCES * (size_t)vertex_count);
    mesh->joints = malloc(sizeof(unsigned char) * SKIN_INFLUENCES * (size_t)vertex_count);
    mesh->indices = malloc(sizeof(unsigned int) * (size_t)indices.count);
    if (has_uvs) mesh->uvs = malloc(sizeof(float) * 2 * (size_t)vertex_count);
    wide = malloc(sizeof(unsigned int) * SKIN_INFLUENCES * (size_t)vertex_count);
    if (!mesh->positions || !mesh->normals || !mesh->weights || !mesh->joints ||
        !mesh->indices || (has_uvs && !mesh->uvs) || !wide) {
        free(wide);
        return glb_fail(error, error_size, "out of memory for %d vertices", vertex_count);
    }

    int ok = glb_read_floats(&position, mesh->positions, error, error_size) &&
             glb_read_floats(&normal, mesh->normals, error, error_size) &&
             glb_read_floats(&weights, mesh->weights, error, error_size) &&
             glb_read_uints(&indices, mesh->indices, (unsigned int)vertex_count,
                            "index", error, error_size) &&
             glb_read_uints(&joints, wide, (unsigned int)joint_count,
                            "joint", error, error_size) &&
             (!has_uvs || glb_read_floats(&uvs, mesh->uvs, error, error_size));
    if (ok) {
        for (int i = 0; i < vertex_count * SKIN_INFLUENCES; ++i)
            mesh->joints[i] = (unsigned char)wide[i];
        normalize_weights(mesh->weights, vertex_count);
        read_material(glb, primitive, mesh);
    }
    free(wide);
    return ok;
}

// Blender names a copy "Name.001"; the suffix means nothing here
static void strip_copy_suffix(char* name) {
	size_t length = strlen(name);
	size_t digits = 0;

	while(digits < length && name[length - 1 - digits] >= '0' &&
	      name[length - 1 - digits] <= '9')
		digits++;

	if(digits > 0 && length - digits >= 2 && name[length - digits - 1] == '.')
		name[length - digits - 1] = '\0';
}

// Read every primitive of one skinned node's mesh into the next free meshes
static int parse_mesh_node(const Glb* glb, SkinnedModel* model, JsonSlice node,
                           JsonSlice meshes, int mesh_index, char* error, int error_size) {
	JsonSlice mesh;
	char name[SKIN_NAME_MAX];

	if(!json_array_at(meshes, mesh_index, &mesh))
		return glb_fail(error, error_size, "mesh %d is missing", mesh_index);

	int count = 0;
	JsonSlice* primitives = index_member_array(mesh, "primitives", &count);

	if(!primitives)
		return glb_fail(error, error_size, "mesh %d has no primitives", mesh_index);

	if(model->mesh_count + count > SKIN_MAX_MESHES) {
		free(primitives);
		return glb_fail(error, error_size, "the model has more than %d primitives",
		                SKIN_MAX_MESHES);
	}

	// The node's name is the object's name in Blender, the one a person sets
	read_name(node, name, sizeof(name));
	if(name[0] == '\0')
		read_name(mesh, name, sizeof(name));
	strip_copy_suffix(name);

	int ok = 1;

	for(int i = 0; i < count && ok; ++i) {
		// Counted first, so a primitive that fails halfway is still freed
		SkinnedMesh* target = &model->meshes[model->mesh_count];

		model->mesh_count++;
		snprintf(target->name, sizeof(target->name), "%s", name);
		ok = parse_primitive(glb, primitives[i], model->skeleton.joint_count,
		                     target, error, error_size);
	}

	free(primitives);
	return ok;
}

// A model that names its bodies measures its height from them alone, so the
// ears standing above the head do not change how tall a character is
static int is_body_mesh(const SkinnedMesh* mesh) {
	return strncmp(mesh->name, "Body_", 5) == 0;
}

static int parse_meshes(const Glb* glb, SkinnedModel* model, char* error, int error_size) {
	JsonSlice nodes, node, meshes;

	if(!json_member(glb->root, "nodes", &nodes))
		return glb_fail(error, error_size, "glb has no nodes");
	if(!json_member(glb->root, "meshes", &meshes))
		return glb_fail(error, error_size, "glb has no meshes");

	// Every node that hangs a mesh on a skin is read; a kit keeps parts apart
	int node_count = json_array_count(nodes);
	int found = 0;

	for(int i = 0; i < node_count; ++i) {
		int mesh_index = 0, skin = 0;

		if(!json_array_at(nodes, i, &node))
			continue;
		if(!read_int_member(node, "mesh", &mesh_index) ||
		   !read_int_member(node, "skin", &skin))
			continue;

		found++;
		if(!parse_mesh_node(glb, model, node, meshes, mesh_index, error, error_size))
			return 0;
	}

	if(!found)
		return glb_fail(error, error_size, "no node in this glb carries a skinned mesh");

	int bodies = 0;

	for(int m = 0; m < model->mesh_count; ++m)
		if(is_body_mesh(&model->meshes[m]))
			bodies++;

	model->min_y = model->max_y = 0;

	int first = 1;

	for(int m = 0; m < model->mesh_count; ++m) {
		const SkinnedMesh* it = &model->meshes[m];

		if(bodies > 0 && !is_body_mesh(it))
			continue;

		for(int i = 0; i < it->vertex_count; ++i) {
			float y = it->positions[i * 3 + 1];

			if(first)
				model->min_y = model->max_y = y;
			first = 0;
			if(y < model->min_y)
				model->min_y = y;
			if(y > model->max_y)
				model->max_y = y;
		}
	}

	return 1;
}

/* ---- animations --------------------------------------------------------- */

static void free_clip(AnimationClip* clip) {
    for (int i = 0; i < clip->sampler_count; ++i) {
        free(clip->samplers[i].times);
        free(clip->samplers[i].values);
    }
    free(clip->samplers);
    free(clip->channels);
    memset(clip, 0, sizeof *clip);
}

static int parse_sampler(const Glb* glb, JsonSlice slice, AnimSampler* out,
                         char* error, int error_size) {
    JsonSlice value;
    char interpolation[16];
    int input = 0, output = 0;
    GlbAccessor times, values;

    /* STEP and CUBICSPLINE would each need their own evaluator, and reading
       one as LINEAR would slur a snap or drop tangents. Both libraries export
       LINEAR throughout, so this refuses rather than approximates. */
    if (json_member(slice, "interpolation", &value) &&
        json_string(value, interpolation, sizeof interpolation) &&
        strcmp(interpolation, "LINEAR") != 0)
        return glb_fail(error, error_size, "sampler interpolation %s is not supported",
                        interpolation);
    if (!read_int_member(slice, "input", &input) || !read_int_member(slice, "output", &output))
        return glb_fail(error, error_size, "animation sampler is missing input or output");
    if (!glb_accessor(glb, input, "SCALAR", 1, &times, error, error_size)) return 0;

    /* Translation and scale are VEC3, rotation VEC4; the sampler itself does
       not say which, so the output accessor's own type decides. */
    if (glb_accessor(glb, output, "VEC3", 3, &values, NULL, 0)) out->components = 3;
    else if (glb_accessor(glb, output, "VEC4", 4, &values, NULL, 0)) out->components = 4;
    else return glb_fail(error, error_size, "animation output %d is neither VEC3 nor VEC4",
                         output);
    if (values.count != times.count)
        return glb_fail(error, error_size, "animation sampler has %d times for %d values",
                        times.count, values.count);

    out->key_count = times.count;
    out->times = malloc(sizeof(float) * (size_t)times.count);
    out->values = malloc(sizeof(float) * (size_t)values.count * (size_t)out->components);
    if (!out->times || !out->values)
        return glb_fail(error, error_size, "out of memory for %d keyframes", times.count);
    return glb_read_floats(&times, out->times, error, error_size) &&
           glb_read_floats(&values, out->values, error, error_size);
}

static int parse_clip(const Glb* glb, JsonSlice slice, const int* remap, int remap_count,
                      AnimationClip* clip, char* error, int error_size) {
    memset(clip, 0, sizeof *clip);
    read_name(slice, clip->name, SKIN_NAME_MAX);

    JsonSlice* samplers = index_member_array(slice, "samplers", &clip->sampler_count);
    JsonSlice* channels = index_member_array(slice, "channels", &clip->channel_count);
    int ok = samplers && channels;
    if (!ok) glb_fail(error, error_size, "animation %s has no samplers or channels", clip->name);

    if (ok) {
        clip->samplers = calloc((size_t)clip->sampler_count, sizeof *clip->samplers);
        clip->channels = calloc((size_t)clip->channel_count, sizeof *clip->channels);
        ok = clip->samplers && clip->channels;
        if (!ok) glb_fail(error, error_size, "out of memory for animation %s", clip->name);
    }
    for (int i = 0; i < clip->sampler_count && ok; ++i)
        ok = parse_sampler(glb, samplers[i], &clip->samplers[i], error, error_size);

    int kept = 0;
    for (int i = 0; i < clip->channel_count && ok; ++i) {
        JsonSlice target, value;
        char path[16];
        int sampler = 0, node = 0;
        if (!read_int_member(channels[i], "sampler", &sampler) ||
            sampler < 0 || sampler >= clip->sampler_count ||
            !json_member(channels[i], "target", &target) ||
            !json_member(target, "path", &value) || !json_string(value, path, sizeof path)) {
            ok = glb_fail(error, error_size, "animation %s has a malformed channel", clip->name);
            break;
        }
        /* A channel with no target node, or one aimed at a bone this skeleton
           does not have, is dropped rather than refused: weights channels and
           a second library's spare props are both harmless to skip. */
        if (!read_int_member(target, "node", &node)) continue;
        if (node < 0 || node >= remap_count || remap[node] < 0) continue;

        unsigned char kind;
        if (strcmp(path, "translation") == 0) kind = ANIM_PATH_TRANSLATION;
        else if (strcmp(path, "rotation") == 0) kind = ANIM_PATH_ROTATION;
        else if (strcmp(path, "scale") == 0) kind = ANIM_PATH_SCALE;
        else continue;

        clip->channels[kept].node = (unsigned short)remap[node];
        clip->channels[kept].sampler = (unsigned short)sampler;
        clip->channels[kept].path = kind;
        kept++;
    }
    clip->channel_count = kept;

    for (int i = 0; i < clip->sampler_count && ok; ++i) {
        const AnimSampler* sampler = &clip->samplers[i];
        if (sampler->key_count > 0 && sampler->times[sampler->key_count - 1] > clip->duration)
            clip->duration = sampler->times[sampler->key_count - 1];
    }

    free(samplers);
    free(channels);
    if (!ok) free_clip(clip);
    return ok;
}

static int parse_animations(const Glb* glb, SkinnedModel* model, const int* remap,
                            int remap_count, char* error, int error_size) {
    int count = 0;
    JsonSlice* table = index_member_array(glb->root, "animations", &count);
    if (!table) return 1;   /* a file with no clips is a valid file */

    AnimationClip* grown = realloc(model->clips,
                                   sizeof *grown * (size_t)(model->clip_count + count));
    if (!grown) {
        free(table);
        return glb_fail(error, error_size, "out of memory for %d clips", count);
    }
    model->clips = grown;

    int ok = 1;
    for (int i = 0; i < count && ok; ++i) {
        ok = parse_clip(glb, table[i], remap, remap_count,
                        &model->clips[model->clip_count], error, error_size);
        if (ok) model->clip_count++;
    }
    free(table);
    return ok;
}

/* ---- entry points ------------------------------------------------------- */

/* Everything a model owns in process memory. GL names are the renderer's to
   delete, before this runs. */
void skinned_model_free(SkinnedModel* model) {
    if (!model) return;
    for (int i = 0; i < model->mesh_count; ++i) {
        SkinnedMesh* mesh = &model->meshes[i];
        free(mesh->positions);
        free(mesh->normals);
        free(mesh->joints);
        free(mesh->weights);
        free(mesh->indices);
        free(mesh->uvs);
        if (mesh->image) stbi_image_free(mesh->image);
    }
    for (int i = 0; i < model->clip_count; ++i) free_clip(&model->clips[i]);
    free(model->clips);
    memset(model, 0, sizeof *model);
}

int skinned_model_load_glb(SkinnedModel* model, const char* path,
                           char* error, int error_size) {
    if (!model) return glb_fail(error, error_size, "no model to fill");
    memset(model, 0, sizeof *model);

    Glb glb;
    if (!glb_open(path, &glb, error, error_size)) return 0;

    /* The file being loaded defines the skeleton, so its node numbering is
       ours and the remap is the identity. */
    int identity[SKIN_MAX_NODES];
    int ok = parse_nodes(&glb, &model->skeleton, error, error_size) &&
             parse_skin(&glb, &model->skeleton, error, error_size) &&
             parse_meshes(&glb, model, error, error_size);
    if (ok) {
        for (int i = 0; i < model->skeleton.node_count; ++i) identity[i] = i;
        ok = parse_animations(&glb, model, identity, model->skeleton.node_count,
                              error, error_size);
    }

    glb_close(&glb);
    if (!ok) skinned_model_free(model);
    return ok;
}

int skinned_model_find_clip(const SkinnedModel* model, const char* name) {
    if (!model || !name) return -1;
    for (int i = 0; i < model->clip_count; ++i)
        if (strcmp(model->clips[i].name, name) == 0) return i;
    return -1;
}
