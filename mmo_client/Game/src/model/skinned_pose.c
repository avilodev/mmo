#include "model/skinned_model.h"

#include <stdlib.h>
#include <string.h>

/* Turning clips into vertices: sample a clip to local bone transforms, blend
   between two of those, then walk the hierarchy and move the mesh.

   The last step reads the model and writes a SkinnedInstance, which is what
   keeps the model an asset rather than a character: the bind pose, the skeleton
   and the clips never change once loaded, so every body wearing this rig shares
   one copy of them, and only the vertices that come out are per body.

   No GL here on purpose. Everything in this file is arithmetic on arrays, so
   it can be run and checked without a window, which is where an animation bug
   is cheapest to find. */

void skinned_model_bind_pose(const SkinnedModel* model, Pose* out) {
    if (!model || !out) return;
    memcpy(out->local, model->skeleton.bind_local,
           sizeof(Transform) * (size_t)model->skeleton.node_count);
}

/* Finds the keyframe interval holding `time` and interpolates across it.
   Binary search rather than a scan because a two second clip at thirty keys a
   second is sixty keys, sampled sixty-five times a frame. */
static void sample_track(const AnimSampler* sampler, float time, float* out) {
    int last = sampler->key_count - 1;
    int components = sampler->components;
    if (last <= 0 || time <= sampler->times[0]) {
        memcpy(out, sampler->values, sizeof(float) * (size_t)components);
        return;
    }
    if (time >= sampler->times[last]) {
        memcpy(out, sampler->values + (size_t)last * (size_t)components,
               sizeof(float) * (size_t)components);
        return;
    }

    int low = 0, high = last;
    while (high - low > 1) {
        int middle = (low + high) / 2;
        if (sampler->times[middle] <= time) low = middle;
        else high = middle;
    }
    float span = sampler->times[high] - sampler->times[low];
    float t = span > 1e-8f ? (time - sampler->times[low]) / span : 0.0f;

    const float* from = sampler->values + (size_t)low * (size_t)components;
    const float* to = sampler->values + (size_t)high * (size_t)components;
    if (components == 4) {
        quat_nlerp(from, to, t, out);
    } else {
        for (int i = 0; i < components; ++i) out[i] = from[i] + (to[i] - from[i]) * t;
    }
}

void skinned_model_sample(const SkinnedModel* model, int clip_index, float time, Pose* out) {
    if (!model || !out) return;
    skinned_model_bind_pose(model, out);
    if (clip_index < 0 || clip_index >= model->clip_count) return;

    /* Starting from the bind pose rather than zeroing means a bone the clip
       says nothing about keeps the shape the artist gave it, which is what
       lets a clip that only animates the upper body leave the legs standing. */
    const AnimationClip* clip = &model->clips[clip_index];
    for (int i = 0; i < clip->channel_count; ++i) {
        const AnimChannel* channel = &clip->channels[i];
        const AnimSampler* sampler = &clip->samplers[channel->sampler];
        if (sampler->key_count <= 0) continue;
        Transform* local = &out->local[channel->node];
        switch (channel->path) {
            case ANIM_PATH_TRANSLATION: sample_track(sampler, time, local->translation); break;
            case ANIM_PATH_ROTATION:    sample_track(sampler, time, local->rotation); break;
            case ANIM_PATH_SCALE:       sample_track(sampler, time, local->scale); break;
            default: break;
        }
    }
}

void skinned_model_blend(const SkinnedModel* model, const Pose* a, const Pose* b,
                         float t, Pose* out) {
    if (!model || !a || !b || !out) return;
    if (t <= 0.0f) { *out = *a; return; }
    if (t >= 1.0f) { *out = *b; return; }
    for (int i = 0; i < model->skeleton.node_count; ++i) {
        const Transform* from = &a->local[i];
        const Transform* to = &b->local[i];
        Transform* result = &out->local[i];
        for (int c = 0; c < 3; ++c) {
            result->translation[c] = from->translation[c] +
                                     (to->translation[c] - from->translation[c]) * t;
            result->scale[c] = from->scale[c] + (to->scale[c] - from->scale[c]) * t;
        }
        quat_nlerp(from->rotation, to->rotation, t, result->rotation);
    }
}

int skinned_instance_init(SkinnedInstance* out, const SkinnedModel* model) {
    if (!out || !model) return 0;
    memset(out, 0, sizeof *out);
    for (int i = 0; i < model->mesh_count; ++i) {
        const SkinnedMesh* mesh = &model->meshes[i];
        SkinnedMeshVertices* target = &out->meshes[i];
        size_t floats = (size_t)mesh->vertex_count * SKIN_VERTEX_FLOATS;
        target->vertices = malloc(floats * sizeof *target->vertices);
        if (!target->vertices) {
            skinned_instance_free(out);
            return 0;
        }
        /* Seeded from the bind pose, so a body drawn before its first skin
           stands in the rest pose rather than in whatever malloc left behind.
           Interleaved on the way in, since that is the layout every later
           write and every upload uses. */
        for (int v = 0; v < mesh->vertex_count; ++v) {
            float* vertex = target->vertices + (size_t)v * SKIN_VERTEX_FLOATS;
            memcpy(vertex, mesh->positions + (size_t)v * 3, sizeof(float) * 3);
            memcpy(vertex + 3, mesh->normals + (size_t)v * 3, sizeof(float) * 3);
        }
        target->vertex_count = mesh->vertex_count;
        out->mesh_count = i + 1;
    }
    return 1;
}

void skinned_instance_free(SkinnedInstance* instance) {
    if (!instance) return;
    for (int i = 0; i < SKIN_MAX_MESHES; ++i) free(instance->meshes[i].vertices);
    /* The GL buffers are the drawing file's to release - this one has no GL in
       it and may be running with no context at all. model_renderer releases
       them first. */
    memset(instance, 0, sizeof *instance);
}

void skinned_model_skin(const SkinnedModel* model, const Pose* pose,
                        SkinnedInstance* instance) {
    if (!model || !pose || !instance) return;
    const Skeleton* skeleton = &model->skeleton;

    /* Local transforms to world-of-the-model transforms, in an order that
       guarantees a parent is done before its children. */
    Mat4 global[SKIN_MAX_NODES];
    for (int i = 0; i < skeleton->node_count; ++i) {
        int node = skeleton->order[i];
        const Transform* local = &pose->local[node];
        Mat4 matrix = mat4_from_trs(local->translation, local->rotation, local->scale);
        int parent = skeleton->parent[node];
        global[node] = parent < 0 ? matrix : mat4_multiply(global[parent], matrix);
    }

    /* The inverse bind matrix undoes the rest pose, so what is left is the
       movement away from it - which is the only part a vertex should follow. */
    Mat4 joint[SKIN_MAX_JOINTS];
    for (int j = 0; j < skeleton->joint_count; ++j)
        joint[j] = mat4_multiply(global[skeleton->joint_node[j]], skeleton->inverse_bind[j]);

    /* An instance built against a different model would index the wrong
       vertices; whichever count is lower is the only part they agree on. */
    int mesh_count = model->mesh_count < instance->mesh_count
        ? model->mesh_count : instance->mesh_count;
    for (int m = 0; m < mesh_count; ++m) {
        const SkinnedMesh* mesh = &model->meshes[m];
        SkinnedMeshVertices* target = &instance->meshes[m];
        int vertex_count = mesh->vertex_count < target->vertex_count
            ? mesh->vertex_count : target->vertex_count;
        for (int v = 0; v < vertex_count; ++v) {
            const unsigned char* bones = mesh->joints + (size_t)v * SKIN_INFLUENCES;
            const float* weights = mesh->weights + (size_t)v * SKIN_INFLUENCES;

            /* The four influences are averaged as matrices and applied once,
               rather than transforming the vertex four times and averaging the
               results. Same answer, two thirds of the multiplies. */
            Mat4 blended;
            memset(&blended, 0, sizeof blended);
            for (int i = 0; i < SKIN_INFLUENCES; ++i) {
                float weight = weights[i];
                if (weight <= 0.0f) continue;
                const float* source = joint[bones[i]].m;
                /* All sixteen. The transforms below read twelve of them and
                   the old fifteen was enough for those, but it left m[15] at
                   zero, so the result was not a matrix any general routine
                   could be handed - mat4_multiply on one would have quietly
                   produced nonsense. The sixteenth multiply-add is not worth
                   that trap. */
                for (int e = 0; e < 16; ++e) blended.m[e] += source[e] * weight;
            }

            float* vertex = target->vertices + (size_t)v * SKIN_VERTEX_FLOATS;
            float* position = vertex;
            float* normal = vertex + 3;
            mat4_transform_point(&blended, mesh->positions + (size_t)v * 3, position);
            mat4_transform_direction(&blended, mesh->normals + (size_t)v * 3, normal);

            /* Averaging rotations shortens the result, and a short normal is a
               dim triangle under fixed-function lighting. */
            float length = normal[0] * normal[0] + normal[1] * normal[1] + normal[2] * normal[2];
            if (length > 1e-12f) {
                float inverse = 1.0f / sqrtf(length);
                normal[0] *= inverse;
                normal[1] *= inverse;
                normal[2] *= inverse;
            }
        }
    }
}

int skinned_model_bone_matrix(const SkinnedModel* model, const Pose* pose,
                              const char* name, Mat4* out) {
    if (!model || !pose || !name || !out) return 0;
    const Skeleton* skeleton = &model->skeleton;

    int node = -1;
    for (int i = 0; i < skeleton->node_count && node < 0; ++i)
        if (strcmp(skeleton->node_names[i], name) == 0) node = i;
    if (node < 0) return 0;

    /* Root first, so the walk down multiplies parent by child in the order
       mat4_multiply expects. The chain cannot be longer than the skeleton,
       which is what stops a malformed parent cycle from running off the end. */
    int chain[SKIN_MAX_NODES];
    int depth = 0;
    for (int i = node; i >= 0 && depth < SKIN_MAX_NODES; i = skeleton->parent[i])
        chain[depth++] = i;

    *out = mat4_identity();
    for (int i = depth - 1; i >= 0; --i) {
        const Transform* local = &pose->local[chain[i]];
        *out = mat4_multiply(*out, mat4_from_trs(local->translation, local->rotation,
                                                 local->scale));
    }
    return 1;
}
