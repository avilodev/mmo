#ifndef MODEL_SKINNED_MODEL_H
#define MODEL_SKINNED_MODEL_H

/**
 * @file
 * A rigged character: skinned meshes, the skeleton they hang off, and the
 * animation clips that move it.
 *
 * Ported from the opengl_3d prototype, where it was built and exercised
 * against the same Quaternius rig this client ships. Loading and posing need
 * no GL context, so they are unit-tested headless (model_test.c); the GL half
 * is render/model_renderer.c.
 *
 * The model is an asset, not a character. The bind pose, the skeleton and the
 * clips are read-only once loaded and shared by every body wearing the rig;
 * each body's skinned vertices live in its own SkinnedInstance.
 *
 * Skinning is on the CPU: one blended matrix per vertex per body per frame.
 * The mannequin is ~8.5k vertices, and character_renderer.c throttles bodies
 * far from the camera.
 */

#include "model/mat4.h"

#define SKIN_MAX_NODES   256
#define SKIN_MAX_JOINTS  128
#define SKIN_MAX_MESHES  24
#define SKIN_INFLUENCES  4     /* glTF's JOINTS_0/WEIGHTS_0 is exactly four */
/** Floats per skinned vertex: position then normal, interleaved. */
#define SKIN_VERTEX_FLOATS 6
#define SKIN_NAME_MAX    64

/** A node's local transform. Rotation is a quaternion (x, y, z, w). */
typedef struct {
    float translation[3];
    float rotation[4];
    float scale[3];
} Transform;

/** One skeleton's worth of local transforms (~10 KB: keep it off deep stacks). */
typedef struct {
    Transform local[SKIN_MAX_NODES];
} Pose;

typedef struct {
    char name[SKIN_NAME_MAX];
    Transform bind_local[SKIN_MAX_NODES];
    int parent[SKIN_MAX_NODES];      /* -1 for a root */
    int order[SKIN_MAX_NODES];       /* node indices, parents before children */
    char node_names[SKIN_MAX_NODES][SKIN_NAME_MAX];
    int node_count;
    int joint_node[SKIN_MAX_JOINTS]; /* joint index -> node index */
    Mat4 inverse_bind[SKIN_MAX_JOINTS];
    int joint_count;
} Skeleton;

typedef struct {
    float* times;                    /* key_count */
    float* values;                   /* key_count * components */
    int key_count;
    int components;                  /* 3 for translation and scale, 4 for rotation */
} AnimSampler;

enum { ANIM_PATH_TRANSLATION, ANIM_PATH_ROTATION, ANIM_PATH_SCALE };

typedef struct {
    unsigned short node;
    unsigned short sampler;
    unsigned char path;
} AnimChannel;

typedef struct {
    char name[SKIN_NAME_MAX];
    float duration;
    AnimSampler* samplers;
    int sampler_count;
    AnimChannel* channels;
    int channel_count;
} AnimationClip;

typedef struct {
    float* positions;                /* bind pose, 3 per vertex */
    float* normals;
    unsigned char* joints;           /* SKIN_INFLUENCES per vertex */
    float* weights;                  /* SKIN_INFLUENCES per vertex */
    unsigned int* indices;
    int vertex_count;
    int index_count;
    float* uvs;                      /* NULL when the file gave none */

    float base_color[3];
    int double_sided;

    /* The base colour map, decoded but not yet on the GPU; NULL draws flat. */
    unsigned char* image;
    int image_width;
    int image_height;

    /* GL names, owned by model_renderer.c; 0 until uploaded. */
    unsigned int texture;
    unsigned int index_buffer;
    unsigned int uv_buffer;
} SkinnedMesh;

typedef struct {
    SkinnedMesh meshes[SKIN_MAX_MESHES];
    int mesh_count;
    Skeleton skeleton;
    AnimationClip* clips;
    int clip_count;
    float min_y, max_y;              /* bind pose height extent, for scaling to fit */
} SkinnedModel;

/** One body's skinned vertices for one mesh of the model. */
typedef struct {
    float* vertices;                 /* SKIN_VERTEX_FLOATS per vertex */
    int vertex_count;
    unsigned int vertex_buffer;      /* GL name, owned by model_renderer.c */
} SkinnedMeshVertices;

/** One body wearing a model. Parallel to that model's meshes. */
typedef struct {
    SkinnedMeshVertices meshes[SKIN_MAX_MESHES];
    int mesh_count;
} SkinnedInstance;

/* ---- loading (skinned_model_glb.c) ------------------------------------- */

/** Read a binary glTF holding one skinned mesh, its skeleton and its clips.
 *  @return Nonzero on success; on failure `error` says why and the model is zeroed. */
int skinned_model_load_glb(SkinnedModel* model, const char* path,
                           char* error, int error_size);

/** Append another file's clips, matching nodes by name. */
int skinned_model_add_clips(SkinnedModel* model, const char* path,
                            char* error, int error_size);

/** A clip's index by name, or -1. */
int skinned_model_find_clip(const SkinnedModel* model, const char* name);

/** Free what the model holds in process memory. Delete its GL names first
 *  (model_renderer_release_model). */
void skinned_model_free(SkinnedModel* model);

/* ---- posing and skinning (skinned_pose.c) ------------------------------- */

/** The skeleton as authored. */
void skinned_model_bind_pose(const SkinnedModel* model, Pose* out);

/** Sample `clip` at `time` seconds, holding the end keys rather than wrapping. */
void skinned_model_sample(const SkinnedModel* model, int clip, float time, Pose* out);

/** out = a moving toward b by t. `out` may alias either input. */
void skinned_model_blend(const SkinnedModel* model, const Pose* a, const Pose* b,
                         float t, Pose* out);

/** Give a body its own vertices, seeded with the bind pose. Nonzero on success. */
int  skinned_instance_init(SkinnedInstance* out, const SkinnedModel* model);
void skinned_instance_free(SkinnedInstance* instance);

/** Pose the skeleton and write the instance's vertices. */
void skinned_model_skin(const SkinnedModel* model, const Pose* pose,
                        SkinnedInstance* instance);

/** Where the named bone is in `pose`, in model space. Nonzero when found. */
int skinned_model_bone_matrix(const SkinnedModel* model, const Pose* pose,
                              const char* name, Mat4* out);

#endif /* MODEL_SKINNED_MODEL_H */
