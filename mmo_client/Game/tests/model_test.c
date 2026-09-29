/**
 * @file
 * Check the shipped character model loads, poses and skins, without OpenGL.
 *
 * The model is data the client reads at startup, and every way it can go
 * wrong -- a missing clip, a rig that no longer fits the height the renderer
 * scales it to, skinning that throws a vertex across the map -- shows up only
 * as a character that is invisible, frozen or exploded. Pinning those here
 * catches them before anybody has to log in to see it.
 *
 * Run from mmo_client/, like the game, so the asset path is the game's.
 */

#include "model/skinned_model.h"
#include "render/character_tuning.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define STB_IMAGE_IMPLEMENTATION
#include "utils/stb_image.h"

static int g_failures = 0;

#define CHECK(cond, what)                                                   \
    do {                                                                    \
        if (cond) {                                                         \
            printf("  ok   %s\n", (what));                                  \
        } else {                                                            \
            printf("  FAIL %s  (%s:%d)\n", (what), __FILE__, __LINE__);     \
            g_failures++;                                                   \
        }                                                                   \
    } while (0)

/** Largest distance any skinned vertex sits from the model's origin. */
static float max_vertex_reach(const SkinnedInstance* inst) {
    float reach = 0.0f;
    for (int m = 0; m < inst->mesh_count; m++) {
        const SkinnedMeshVertices* v = &inst->meshes[m];
        for (int i = 0; i < v->vertex_count; i++) {
            const float* p = v->vertices + (size_t)i * SKIN_VERTEX_FLOATS;
            if (!isfinite(p[0]) || !isfinite(p[1]) || !isfinite(p[2])) return INFINITY;
            float d = sqrtf(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
            if (d > reach) reach = d;
        }
    }
    return reach;
}

/** Whether two instances differ anywhere by more than `eps`. */
static int instances_differ(const SkinnedInstance* a, const SkinnedInstance* b, float eps) {
    for (int m = 0; m < a->mesh_count; m++) {
        for (int i = 0; i < a->meshes[m].vertex_count * SKIN_VERTEX_FLOATS; i++) {
            if (fabsf(a->meshes[m].vertices[i] - b->meshes[m].vertices[i]) > eps) return 1;
        }
    }
    return 0;
}

int main(void) {
    printf("=== character model ===\n");

    static SkinnedModel model;
    char error[256] = "";

    printf("\nTEST 1: a missing file fails with a reason, not a crash\n");
    {
        static SkinnedModel missing;
        int ok = skinned_model_load_glb(&missing, "Game/assets/models/nope.glb",
                                        error, (int)sizeof(error));
        CHECK(!ok, "load of a missing file fails");
        CHECK(error[0] != '\0', "and says why");
        CHECK(missing.mesh_count == 0 && missing.clip_count == 0, "and leaves the model empty");
    }

    printf("\nTEST 2: the shipped mannequin loads\n");
    int loaded = skinned_model_load_glb(&model, CHARACTER_MODEL_PATH, error, (int)sizeof(error));
    if (!loaded) printf("  (%s)\n", error);
    CHECK(loaded, CHARACTER_MODEL_PATH " loads");
    if (!loaded) {
        printf("\n%d failure(s)\n", g_failures);
        return 1;
    }
    CHECK(model.mesh_count >= 1, "has at least one mesh");
    CHECK(model.skeleton.joint_count > 0 && model.skeleton.joint_count <= SKIN_MAX_JOINTS,
          "has a skeleton that fits");
    CHECK(model.max_y - model.min_y > 1.5f && model.max_y - model.min_y < 2.2f,
          "stands about 1.8 model units tall (the renderer scales from this)");

    printf("\nTEST 3: every clip the characters play is in the file\n");
    {
        const char* needed[] = {
            CHARACTER_CLIP_IDLE, CHARACTER_CLIP_WALK, CHARACTER_CLIP_JOG, CHARACTER_CLIP_RUN,
            CHARACTER_CLIP_TALK, CHARACTER_CLIP_DEATH,
        };
        for (size_t i = 0; i < sizeof(needed) / sizeof(needed[0]); i++) {
            int clip = skinned_model_find_clip(&model, needed[i]);
            char what[96];
            snprintf(what, sizeof(what), "%s is present and has length", needed[i]);
            CHECK(clip >= 0 && model.clips[clip].duration > 0.0f, what);
        }
        CHECK(skinned_model_find_clip(&model, "No_Such_Clip") == -1, "an unknown clip is -1");
    }

    printf("\nTEST 4: skinning moves the body and keeps it in one piece\n");
    {
        SkinnedInstance bind, walking;
        int ok = skinned_instance_init(&bind, &model) && skinned_instance_init(&walking, &model);
        CHECK(ok, "two instances initialise");

        static Pose pose;
        skinned_model_bind_pose(&model, &pose);
        skinned_model_skin(&model, &pose, &bind);

        int walk = skinned_model_find_clip(&model, CHARACTER_CLIP_WALK);
        skinned_model_sample(&model, walk, model.clips[walk].duration * 0.25f, &pose);
        skinned_model_skin(&model, &pose, &walking);

        CHECK(instances_differ(&bind, &walking, 1e-3f), "a walk frame differs from the bind pose");
        float reach = max_vertex_reach(&walking);
        CHECK(isfinite(reach) && reach < 3.0f, "no vertex is thrown away from the body");

        /* Blending halfway lands between the two, not on either. */
        static Pose bind_pose, blended;
        skinned_model_bind_pose(&model, &bind_pose);
        skinned_model_blend(&model, &bind_pose, &pose, 0.5f, &blended);
        SkinnedInstance half;
        ok = skinned_instance_init(&half, &model);
        skinned_model_skin(&model, &blended, &half);
        CHECK(ok && instances_differ(&half, &bind, 1e-3f) && instances_differ(&half, &walking, 1e-3f),
              "a half blend is neither end");

        skinned_instance_free(&half);
        skinned_instance_free(&walking);
        skinned_instance_free(&bind);
    }

    skinned_model_free(&model);
    CHECK(model.mesh_count == 0 && model.clips == NULL, "free leaves the model empty");

    printf("\n%s: %d failure(s)\n", g_failures ? "FAILED" : "PASSED", g_failures);
    return g_failures ? 1 : 0;
}
