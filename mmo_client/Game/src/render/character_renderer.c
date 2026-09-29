/**
 * @file
 * Keep one animated body per visible entity and draw it with the shared model.
 */

#include "render/character_renderer.h"
#include "render/character_tuning.h"
#include "render/model_renderer.h"
#include "model/skinned_model.h"
#include "camera/cglm_config.h"
#include "core/client_log.h"
#include "renderer.h"
#include "render/world_light.h"

#include <math.h>
#include <string.h>

/** One entity's body: its animation state and its skinned vertices. */
typedef struct {
    uint64_t        key;          /**< 0 for a free slot. */
    int             last_seen;    /**< Frame it was last drawn. */
    CharacterMotion motion;
    SkinnedInstance instance;
    float           skin_wait;    /**< Seconds since last skinned (far bodies). */
    int             skinned;      /**< Has been skinned at least once. */
} Body;

static SkinnedModel g_model;
static int          g_ready;
static int          g_clip[CHARACTER_ANIM_COUNT];
static float        g_duration[CHARACTER_ANIM_COUNT];

static Body g_bodies[CHARACTER_MAX_BODIES];
static int  g_frame;

/* This frame's camera and clock, from character_renderer_begin(). */
static CameraView g_view;
static float      g_focus_x, g_focus_y, g_dt;

/* Scratch poses: ~10 KB each, so static rather than on the stack. */
static Pose g_pose_a, g_pose_b;

static const char* CLIP_NAMES[CHARACTER_ANIM_COUNT] = {
    [CHARACTER_ANIM_IDLE] = CHARACTER_CLIP_IDLE,
    [CHARACTER_ANIM_WALK] = CHARACTER_CLIP_WALK,
    [CHARACTER_ANIM_RUN]  = CHARACTER_CLIP_RUN,
    [CHARACTER_ANIM_TALK] = CHARACTER_CLIP_TALK,
    [CHARACTER_ANIM_JOG]  = CHARACTER_CLIP_JOG,
};

int character_renderer_init(void) {
    char error[256] = "";
    if (!skinned_model_load_glb(&g_model, CHARACTER_MODEL_PATH, error, (int)sizeof(error))) {
        CLOG_ERROR("[CHARACTER] Could not load %s: %s", CHARACTER_MODEL_PATH, error);
        return 0;
    }
    if (!model_renderer_init()) {
        skinned_model_free(&g_model);
        return 0;
    }
    model_renderer_upload(&g_model);

    for (int a = 0; a < CHARACTER_ANIM_COUNT; a++) {
        g_clip[a] = skinned_model_find_clip(&g_model, CLIP_NAMES[a]);
        g_duration[a] = (g_clip[a] >= 0) ? g_model.clips[g_clip[a]].duration : 0.0f;
        if (g_clip[a] < 0)
            CLOG_WARN("[CHARACTER] Clip %s missing; that state holds the bind pose",
                      CLIP_NAMES[a]);
    }

    memset(g_bodies, 0, sizeof(g_bodies));
    g_ready = 1;
    CLOG_INFO("[CHARACTER] Model ready: %d meshes, %d joints, %d clips",
              g_model.mesh_count, g_model.skeleton.joint_count, g_model.clip_count);
    return 1;
}

static void body_release(Body* b) {
    model_renderer_release_instance(&b->instance);
    skinned_instance_free(&b->instance);
    memset(b, 0, sizeof(*b));
}

void character_renderer_shutdown(void) {
    if (!g_ready) return;
    for (int i = 0; i < CHARACTER_MAX_BODIES; i++)
        if (g_bodies[i].key) body_release(&g_bodies[i]);
    model_renderer_release_model(&g_model);
    skinned_model_free(&g_model);
    model_renderer_shutdown();
    g_ready = 0;
}

int character_renderer_ready(void) {
    return g_ready;
}

void character_renderer_set_focus(float x, float y) {
    g_focus_x = x;
    g_focus_y = y;
}

float character_renderer_fog_at(float x, float y) {
    return world_fog(x - g_focus_x, y - g_focus_y);
}

void character_draw_shadow(float x, float y, float scale) {
    /* Fades with the ground it lies on, or it would float in the fog. */
    float clear = 1.0f - character_renderer_fog_at(x, y);
    if (clear <= 0.0f) return;
    renderer_draw_circle(x, y, CHARACTER_SHADOW_RADIUS * scale, 0.0f, 0.0f, 0.0f,
                         CHARACTER_SHADOW_ALPHA * clear, 20);
}

void character_renderer_begin(const CameraView* view, float focus_x, float focus_y, float dt) {
    g_view    = *view;
    g_focus_x = focus_x;
    g_focus_y = focus_y;
    g_dt      = (dt > 0.1f) ? 0.1f : dt;   /* a hitch should not fling a clip forward */
    g_frame++;
    if (g_ready) model_renderer_begin(focus_x, focus_y);
}

/** The body for a key, claiming a free (or the stalest) slot for a new one. */
static Body* body_for(uint64_t key, float x, float y) {
    Body* free_slot = NULL;
    Body* stalest   = NULL;
    for (int i = 0; i < CHARACTER_MAX_BODIES; i++) {
        Body* b = &g_bodies[i];
        if (b->key == key) return b;
        if (!b->key) { if (!free_slot) free_slot = b; continue; }
        if (!stalest || b->last_seen < stalest->last_seen) stalest = b;
    }

    Body* b = free_slot;
    if (!b) {
        /* Never steal a body already drawn this frame: two entities would
         * share one set of vertices. */
        if (!stalest || stalest->last_seen == g_frame) return NULL;
        body_release(stalest);
        b = stalest;
    }
    if (!skinned_instance_init(&b->instance, &g_model)) {
        CLOG_WARN("[CHARACTER] Out of memory for a body");
        return NULL;
    }
    b->key = key;
    character_motion_reset(&b->motion, x, y, 0.0f);
    b->skinned   = 0;
    b->skin_wait = 0.0f;
    return b;
}

static void sample_clip(CharacterAnim anim, float time, Pose* out) {
    if (g_clip[anim] >= 0) skinned_model_sample(&g_model, g_clip[anim], time, out);
    else                   skinned_model_bind_pose(&g_model, out);
}

/** Whether a body at (x, y) could show on screen: its middle, projected, is
 *  within the view plus a margin wide enough for a whole body at the edge. */
static int body_on_screen(float x, float y, float height) {
    vec4 p = { x, height * 0.5f, y, 1.0f }, clip;
    glm_mat4_mulv((vec4*)g_view.viewproj, p, clip);
    if (clip[3] <= 1e-6f) return 0;
    const float margin = 1.25f;
    return fabsf(clip[0]) <= clip[3] * margin && fabsf(clip[1]) <= clip[3] * margin;
}

/** Pose and skin a body, unless it is far away and was skinned recently. */
static void body_skin(Body* b, float x, float y) {
    float dx = x - g_focus_x, dy = y - g_focus_y;
    int far = dx * dx + dy * dy > CHARACTER_FULL_RATE_RADIUS * CHARACTER_FULL_RATE_RADIUS;

    b->skin_wait += g_dt;
    if (far && b->skinned && b->skin_wait < 1.0f / CHARACTER_FAR_SKIN_HZ) return;
    b->skin_wait = 0.0f;

    const CharacterMotion* m = &b->motion;
    sample_clip(m->anim, m->anim_time, &g_pose_a);
    if (m->blend < 1.0f) {
        sample_clip(m->prev, m->prev_time, &g_pose_b);
        skinned_model_blend(&g_model, &g_pose_b, &g_pose_a, m->blend, &g_pose_a);
    }
    skinned_model_skin(&g_model, &g_pose_a, &b->instance);
    b->skinned = 1;
}

void character_renderer_draw(CharacterKind kind, uint32_t id, float x, float y,
                             const CharacterStyle* style) {
    if (!g_ready) return;

    uint64_t key = ((uint64_t)kind << 32) | id;
    Body* b = body_for(key, x, y);
    if (!b) return;
    b->last_seen = g_frame;

    /* The clock runs whether or not the body is seen, so it does not jump
     * when it walks back into view; only the skinning and drawing are skipped. */
    character_motion_update(&b->motion, x, y, g_dt, &style->hints, g_duration);
    if (!body_on_screen(x, y, CHARACTER_HEIGHT * style->scale)) return;
    body_skin(b, x, y);

    /* World (x, y) stands at GL (x, 0, y); the rig's feet go on the ground and
     * its height is scaled to CHARACTER_HEIGHT (D1-D3). */
    float rig_height = g_model.max_y - g_model.min_y;
    float s = (rig_height > 1e-4f) ? CHARACTER_HEIGHT * style->scale / rig_height : 1.0f;

    mat4 model, mvp;
    glm_mat4_identity(model);
    glm_translate(model, (vec3){ x, style->lift, y });
    glm_rotate_y(model, b->motion.yaw, model);
    glm_scale_uni(model, s);
    glm_translate(model, (vec3){ 0.0f, -g_model.min_y, 0.0f });
    glm_mat4_mul((vec4*)g_view.viewproj, model, mvp);

    model_renderer_draw(&g_model, &b->instance, (const float*)mvp, (const float*)model,
                        style->tint);
}

void character_renderer_end(void) {
    if (!g_ready) return;
    model_renderer_end();

    for (int i = 0; i < CHARACTER_MAX_BODIES; i++) {
        Body* b = &g_bodies[i];
        if (b->key && g_frame - b->last_seen > CHARACTER_FORGET_FRAMES)
            body_release(b);
    }
}
