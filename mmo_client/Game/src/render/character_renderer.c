/**
 * @file
 * Keep one animated body per visible entity and draw it with the shared model.
 */

#include "render/character_renderer.h"
#include "render/character_tuning.h"
#include "render/character_look.h"
#include "render/character_pose.h"
#include "render/character_feet.h"
#include "render/character_foot_ik.h"
#include "render/character_legs.h"
#include "render/character_retarget.h"
#include "render/character_spring.h"
#include "protocol.h"
#include "player/emote_clips.h"
#include "world/footing.h"
#include "world/weather_now.h"
#include "world/wind_gust.h"
#include "render/model_renderer.h"
#include "model/skinned_model.h"
#include "camera/cglm_config.h"
#include "core/client_log.h"
#include "renderer.h"
#include "render/world_light.h"
#include "world_spans.h"
#include "world_spans_format.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

// The slots whose clip the emote hub can change, in the order a Body keeps them
enum { OVER_WALK, OVER_JOG, OVER_SIT, OVER_CUSTOM, OVER_COUNT };

/** One entity's body: its animation state and its skinned vertices. */
typedef struct {
    uint64_t        key;          /**< 0 for a free slot. */
    int             last_seen;    /**< Frame it was last drawn. */
    CharacterMotion motion;
    SkinnedInstance instance;
    float           skin_wait;    /**< Seconds since last skinned (far bodies). */
    int             skinned;      /**< Has been skinned at least once. */
	character_feet  feet;         // Foot IK smoothing state
	float           feet_lift;    // How far the skinned legs lift the body
	SkinMeshMask    mask;         // The meshes it wears; instance holds vertices for these
	int             look_set;     // look_key and look_appearance were worn
	char            look_key[32]; // The race key the mask was chosen for
	uint32_t        look_appearance;
	int             look_animal;  // The animal features setting the mask was chosen under
	int             model;        // Which of g_models the instance was made for
	unsigned char   over[OVER_COUNT]; // The hub's clips for this body, as in CharacterHints
	character_spring_state hair;  // Its hair's swing
	character_spring_state tail;  // Its tail's swing
	int             shadow_mark;  // Drawn since the last shadow pass collected it
	float           shadow_model[16]; // Where it stood then, for the shadow
} Body;

// The models a body can wear. The kit holds the clips, and is the NPCs' body.
enum { MODEL_KIT, MODEL_MALE, MODEL_FEMALE, MODEL_COUNT };

typedef struct {
	SkinnedModel      model;
	int               ready;
	character_rig     rig;
	character_legs_rig legs;
	int               legs_ok;
	character_retarget retarget;  // Unused by the kit, which the clips are made on
	character_spring_rig hair;    // The hair bones that swing; none on the kit
	character_spring_rig tail;    // The tail bones that swing
} CharacterModel;

static CharacterModel g_models[MODEL_COUNT];
#define KIT_MODEL (g_models[MODEL_KIT].model)

static int          g_ready;
static int          g_clip[CHARACTER_ANIM_COUNT];
static int          g_table_clip[EMOTE_CLIP_COUNT]; // The kit's clip for each emote clip, or -1
static float        g_duration[CHARACTER_ANIM_COUNT];

static Body g_bodies[CHARACTER_MAX_BODIES];
static int  g_frame;

/* This frame's camera and clock, from character_renderer_begin(). */
static CameraView g_view;
static float      g_focus_x, g_focus_y, g_dt;

/* Scratch poses: ~10 KB each, so static rather than on the stack. */
static Pose g_pose_a, g_pose_b;
// The kit's pose played on another model
static Pose g_pose_t;

static const char* CLIP_NAMES[CHARACTER_ANIM_COUNT] = {
    [CHARACTER_ANIM_IDLE] = CHARACTER_CLIP_IDLE,
    [CHARACTER_ANIM_WALK] = CHARACTER_CLIP_WALK,
    [CHARACTER_ANIM_RUN]  = CHARACTER_CLIP_RUN,
    [CHARACTER_ANIM_TALK] = CHARACTER_CLIP_TALK,
    [CHARACTER_ANIM_JOG]  = CHARACTER_CLIP_JOG,
	[CHARACTER_ANIM_SIT_ENTER] = CHARACTER_CLIP_SIT_ENTER,
	[CHARACTER_ANIM_SIT_IDLE]  = CHARACTER_CLIP_SIT_IDLE,
	[CHARACTER_ANIM_SIT_EXIT]  = CHARACTER_CLIP_SIT_EXIT,
	[CHARACTER_ANIM_DANCE]     = CHARACTER_CLIP_DANCE,
	[CHARACTER_ANIM_FIDGET]    = CHARACTER_CLIP_FIDGET,
	[CHARACTER_ANIM_PIVOT_LEFT]  = CHARACTER_CLIP_PIVOT_LEFT,
	[CHARACTER_ANIM_PIVOT_RIGHT] = CHARACTER_CLIP_PIVOT_RIGHT,
	[CHARACTER_ANIM_LEAN_ENTER]      = CHARACTER_CLIP_LEAN_ENTER,
	[CHARACTER_ANIM_LEAN_IDLE]       = CHARACTER_CLIP_LEAN_IDLE,
	[CHARACTER_ANIM_LEAN_EXIT]       = CHARACTER_CLIP_LEAN_EXIT,
	[CHARACTER_ANIM_LEAN_FOOT_ENTER] = CHARACTER_CLIP_LEAN_FOOT_ENTER,
	[CHARACTER_ANIM_LEAN_FOOT_IDLE]  = CHARACTER_CLIP_LEAN_FOOT_IDLE,
	[CHARACTER_ANIM_LEAN_FOOT_EXIT]  = CHARACTER_CLIP_LEAN_FOOT_EXIT,
	[CHARACTER_ANIM_FIDGET_GLANCE]   = CHARACTER_CLIP_FIDGET_GLANCE,
	[CHARACTER_ANIM_FIDGET_KNUCKLES] = CHARACTER_CLIP_FIDGET_KNUCKLES,
	[CHARACTER_ANIM_FIDGET_SLEEVE]   = CHARACTER_CLIP_FIDGET_SLEEVE,
};

// Where a slot's clip is overridden in a body, or -1 for a slot that has no override
static int over_index(CharacterAnim anim) {
	switch(anim) {
	case CHARACTER_ANIM_WALK:
		return OVER_WALK;
	case CHARACTER_ANIM_JOG:
		return OVER_JOG;
	case CHARACTER_ANIM_SIT_IDLE:
		return OVER_SIT;
	case CHARACTER_ANIM_CUSTOM:
	case CHARACTER_ANIM_CUSTOM_ENTER:
	case CHARACTER_ANIM_CUSTOM_EXIT:
		return OVER_CUSTOM;
	default:
		return -1;
	}
}

// The kit's clip a body plays for a slot: the hub's pick, else the slot's own
static int body_clip(const Body* b, CharacterAnim anim) {
	int slot = over_index(anim);

	if(slot >= 0 && b->over[slot] > 0 && b->over[slot] <= EMOTE_CLIP_COUNT &&
	   g_table_clip[b->over[slot] - 1] >= 0)
		return g_table_clip[b->over[slot] - 1];

	return g_clip[anim];
}

// Each slot's clip length for a body, with its picks in place of the defaults
static void body_durations(const Body* b, float out[CHARACTER_ANIM_COUNT]) {
	for(int a = 0; a < CHARACTER_ANIM_COUNT; a++) {
		int clip = body_clip(b, (CharacterAnim)a);

		out[a] = clip >= 0 ? KIT_MODEL.clips[clip].duration : 0.0f;
	}
}

float character_renderer_clip_seconds(CharacterAnim anim) {
	if(anim < 0 || anim >= CHARACTER_ANIM_COUNT)
		return 0.0f;
	return g_duration[anim];
}

float character_renderer_table_clip_seconds(int clip) {
	if(clip < 0 || clip >= EMOTE_CLIP_COUNT || g_table_clip[clip] < 0)
		return 0.0f;

	return KIT_MODEL.clips[g_table_clip[clip]].duration;
}

// Find the bones the body layer and the foot IK move
static void find_rigs(CharacterModel* cm) {
	if(!character_rig_find(&cm->model, &cm->rig))
		CLOG_WARN("[CHARACTER] A spine, neck or head bone is missing; "
		          "the body layer skips it");

	cm->legs_ok = character_legs_rig_find(&cm->model, &cm->legs);
	if(!cm->legs_ok)
		CLOG_WARN("[CHARACTER] A leg bone is missing; "
		          "feet will not follow stairs");
}

// Find the bones of a model that swing, and the body the hair is kept out of
static void find_springs(CharacterModel* cm) {
	static const char* const COLLIDER_BONES[] = CHARACTER_COLLIDER_BONES;

	character_spring_find(&cm->model, CHARACTER_HAIR_PREFIX, &cm->hair);
	character_spring_find(&cm->model, CHARACTER_TAIL_PREFIX, &cm->tail);
	if(CHARACTER_HAIR_COLLIDE && cm->hair.count > 0)
		character_spring_fit_colliders(&cm->model, COLLIDER_BONES,
		                               (int)(sizeof(COLLIDER_BONES) / sizeof(COLLIDER_BONES[0])),
		                               CHARACTER_COLLIDER_SPACING, CHARACTER_COLLIDER_RADIUS,
		                               CHARACTER_COLLIDER_MARGIN, &cm->hair);
}

// Load one of the players' models. If it will not load, the kit stands in.
static void load_player_model(int slot, const char* path) {
	CharacterModel* cm = &g_models[slot];
	char error[256] = "";

	if(!skinned_model_load_glb(&cm->model, path, error, (int)sizeof(error))) {
		CLOG_WARN("[CHARACTER] Could not load %s: %s; the mannequin stands in", path, error);
		return;
	}

	if(character_retarget_build(&KIT_MODEL, &cm->model, &cm->retarget) == 0) {
		CLOG_WARN("[CHARACTER] %s shares no bones with the kit; the mannequin stands in", path);
		skinned_model_free(&cm->model);
		return;
	}

	model_renderer_upload(&cm->model);
	find_rigs(cm);
	find_springs(cm);
	cm->ready = 1;
	CLOG_INFO("[CHARACTER] %s ready: %d meshes, %d joints, %d twist bones, %d hair, %d tail, "
	          "%d colliders", path, cm->model.mesh_count, cm->model.skeleton.joint_count,
	          cm->retarget.twist_count, cm->hair.count, cm->tail.count, cm->hair.collider_count);
}

int character_renderer_init(void) {
	CharacterModel* kit = &g_models[MODEL_KIT];
	char error[256] = "";

	if(!skinned_model_load_glb(&kit->model, CHARACTER_MODEL_PATH, error, (int)sizeof(error))) {
		CLOG_ERROR("[CHARACTER] Could not load %s: %s", CHARACTER_MODEL_PATH, error);
		return 0;
	}
	if(!model_renderer_init()) {
		skinned_model_free(&kit->model);
		return 0;
	}
	model_renderer_upload(&kit->model);

	for(int a = 0; a < CHARACTER_ANIM_COUNT; a++) {
		// The action slots have no clip until one is picked
		if(!CLIP_NAMES[a]) {
			g_clip[a] = -1;
			g_duration[a] = 0.0f;
			continue;
		}
		g_clip[a] = skinned_model_find_clip(&kit->model, CLIP_NAMES[a]);
		g_duration[a] = (g_clip[a] >= 0) ? kit->model.clips[g_clip[a]].duration : 0.0f;
		if(g_clip[a] < 0)
			CLOG_WARN("[CHARACTER] Clip %s missing; that state holds the bind pose",
			          CLIP_NAMES[a]);
	}

	for(int c = 0; c < EMOTE_CLIP_COUNT; c++)
		g_table_clip[c] = skinned_model_find_clip(&kit->model, emote_clip_name(c));

	find_rigs(kit);
	kit->ready = 1;

	load_player_model(MODEL_MALE, CHARACTER_MODEL_MALE_PATH);
	load_player_model(MODEL_FEMALE, CHARACTER_MODEL_FEMALE_PATH);

	memset(g_bodies, 0, sizeof(g_bodies));
	g_ready = 1;
	CLOG_INFO("[CHARACTER] Model ready: %d meshes, %d joints, %d clips",
	          kit->model.mesh_count, kit->model.skeleton.joint_count, kit->model.clip_count);
	return 1;
}

static void body_release(Body* b) {
    model_renderer_release_instance(&b->instance);
    skinned_instance_free(&b->instance);
    memset(b, 0, sizeof(*b));
}

void character_renderer_reset(void) {
    if (!g_ready) return;
    for (int i = 0; i < CHARACTER_MAX_BODIES; i++)
        if (g_bodies[i].key) body_release(&g_bodies[i]);
}

int character_renderer_body_count(void) {
    int count = 0;

    for (int i = 0; i < CHARACTER_MAX_BODIES; i++)
        if (g_bodies[i].key) count++;
    return count;
}

// Free the GL names and the memory of every model that loaded
static void release_models(void) {
	for(int m = 0; m < MODEL_COUNT; m++) {
		if(!g_models[m].ready)
			continue;
		model_renderer_release_model(&g_models[m].model);
		skinned_model_free(&g_models[m].model);
		g_models[m].ready = 0;
	}
}

void character_renderer_shutdown(void) {
    if (!g_ready) return;
	// A body keeps only its own vertices, so the models can go first
	release_models();
    for (int i = 0; i < CHARACTER_MAX_BODIES; i++)
        if (g_bodies[i].key) body_release(&g_bodies[i]);
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

int character_renderer_in_label_range(float x, float y) {
    return world_in_label_range(x - g_focus_x, y - g_focus_y);
}

/** Where the shadow lies: the highest floor at or below a body at height z,
 *  or z itself when no span says otherwise. */
static float shadow_floor(float x, float y, float z) {
    float floor_z = z;

    if (world_spans_loaded() &&
        world_spans_floor_between(x, y, 0.0f, z - 600.0f, z + WORLD_STEP_HEIGHT, &floor_z))
        return floor_z;
    return z;
}

void character_draw_shadow(float x, float y, float z, float scale) {
    /* A body well above its floor (a jump) casts a smaller shadow below it. */
    float floor_z = shadow_floor(x, y, z);
    float gap = z - floor_z;
    if (gap > 0.0f) 
        scale *= CHARACTER_HEIGHT / (CHARACTER_HEIGHT + gap);

    renderer_lift_begin(floor_z);
    renderer_draw_circle(x, y, CHARACTER_SHADOW_RADIUS * scale, 0.0f, 0.0f, 0.0f,
                         CHARACTER_SHADOW_ALPHA, 20);
    renderer_lift_end();
}

void character_renderer_begin(const CameraView* view, float focus_x, float focus_y, float dt) {
    g_view    = *view;
    g_focus_x = focus_x;
    g_focus_y = focus_y;
    g_dt      = (dt > 0.1f) ? 0.1f : dt;   /* a hitch should not fling a clip forward */
    g_frame++;
    if (g_ready) 
        model_renderer_begin();
}

/** The body for a key, claiming a free (or the stalest) slot for a new one. */
static Body* body_for(uint64_t key, float x, float y) {
    Body* free_slot = NULL;
    Body* stalest   = NULL;
    for (int i = 0; i < CHARACTER_MAX_BODIES; i++) {
        Body* b = &g_bodies[i];
        if (b->key == key) 
            return b;
        if (!b->key) { 
            if (!free_slot) 
                free_slot = b; 
                continue; 
            }
        if (!stalest || b->last_seen < stalest->last_seen) 
            stalest = b;
    }

    Body* b = free_slot;
    if (!b) {
        /* Never steal a body already drawn this frame: two entities would
         * share one set of vertices. */
        if (!stalest || stalest->last_seen == g_frame) 
            return NULL;
        body_release(stalest);
        b = stalest;
    }
	// Vertices come once body_wear knows which meshes the body wears
    if (!skinned_instance_init_masked(&b->instance, &KIT_MODEL, 0)) {
        CLOG_WARN("[CHARACTER] Out of memory for a body");
        return NULL;
    }
    b->key = key;
    character_motion_reset(&b->motion, x, y, 0.0f);
    b->skinned   = 0;
    b->skin_wait = 0.0f;
    return b;
}

// Whether bodies wear their animal features (the setting); changing it dresses them again
static int g_show_animal = 1;

void character_renderer_set_animal_features(int show) {
	g_show_animal = show ? 1 : 0;
}

// The mask without the meshes of the ears and the tail
static SkinMeshMask mask_without_ears_tail(const SkinnedModel* model, SkinMeshMask mask) {
	for(int m = 0; m < model->mesh_count; m++) {
		const char* name = model->meshes[m].name;

		if(strncmp(name, "Ears_", 5) == 0 || strncmp(name, "Tail_", 5) == 0)
			mask &= ~((SkinMeshMask)1 << m);
	}

	return mask;
}

// Which model a body wears: a player their own by body, an NPC the kit's mannequin
static int model_for(uint64_t key, const CharacterStyle* style) {
	int slot = MODEL_KIT;

	if((CharacterKind)(key >> 32) != CHARACTER_KIND_NPC)
		slot = appearance_body(style->appearance) == APPEARANCE_BODY_FEMALE ?
		       MODEL_FEMALE : MODEL_MALE;

	return g_models[slot].ready ? slot : MODEL_KIT;
}

// Dress a body in the model and meshes its look names. The vertices are made again
// only when the look changes, which for a player in the world is never.
static void body_wear(Body* b, const CharacterStyle* style) {
	const char* key = style->race_key ? style->race_key : "";
	int slot = model_for(b->key, style);
	const SkinnedModel* model = &g_models[slot].model;

	if(b->look_set && b->model == slot && b->look_appearance == style->appearance &&
	   b->look_animal == g_show_animal && strncmp(b->look_key, key, sizeof(b->look_key)) == 0)
		return;

	SkinMeshMask mask = character_look_mask(model, style->race_key, style->appearance, 0);

	if(!CHARACTER_SHOW_EARS_TAIL)
		mask = mask_without_ears_tail(model, mask);

	if(!g_show_animal)
		mask = character_look_hide_animal(model, mask);

	b->look_set = 1;
	b->look_animal = g_show_animal;
	b->look_appearance = style->appearance;
	snprintf(b->look_key, sizeof(b->look_key), "%s", key);

	if(b->mask == mask && b->model == slot && b->instance.mesh_count == model->mesh_count &&
	   mask != 0)
		return;

	model_renderer_release_instance(&b->instance);
	skinned_instance_free(&b->instance);
	b->mask = mask;
	b->model = slot;
	b->skinned = 0;
	character_spring_reset(&b->hair);
	character_spring_reset(&b->tail);

	if(!skinned_instance_init_masked(&b->instance, model, mask))
		CLOG_WARN("[CHARACTER] Out of memory for a body's parts");
}

static void sample_clip(const Body* b, CharacterAnim anim, float time, Pose* out) {
	int clip = body_clip(b, anim);

	if(clip >= 0)
		skinned_model_sample(&KIT_MODEL, clip, time, out);
	else
		skinned_model_bind_pose(&KIT_MODEL, out);
}

/** Whether a body at (x, y) could show on screen: its middle, projected, is
 *  within the view plus a margin wide enough for a whole body at the edge. */
static int body_on_screen(float x, float y, float z, float height) {
    vec4 p = { x, z + height * 0.5f, y, 1.0f }, clip;
    glm_mat4_mulv((vec4*)g_view.viewproj, p, clip);
    if (clip[3] <= 1e-6f) return 0;
    const float margin = 1.25f;
    return fabsf(clip[0]) <= clip[3] * margin && fabsf(clip[1]) <= clip[3] * margin;
}

// World units per rig unit for a body of this style (D1-D3). Every model is
// measured against the kit's mannequin, so a shorter one stands shorter.
static float body_scale(const CharacterStyle* style) {
	float rig_height = KIT_MODEL.max_y - KIT_MODEL.min_y;

	if(rig_height <= 1e-4f)
		return 1.0f;

	return CHARACTER_HEIGHT * style->scale / rig_height;
}

// How much foot IK this body gets (see character_feet_want)
static float feet_want(const CharacterModel* cm, const CharacterStyle* style, float x, float y) {
	float dx = x - g_focus_x;
	float dy = y - g_focus_y;

	return character_feet_want(cm->legs_ok && footing_loaded(), style->hints.has_pose,
	                           style->hints.airborne, style->lift, sqrtf(dx * dx + dy * dy));
}

// Plant the feet: bend the legs in g_pose_a and set the body's lift
static void apply_feet(Body* b, const CharacterModel* cm, Pose* pose, float x, float y,
                       float floor_z, const CharacterStyle* style, float dt) {
	character_legs_clip clip;
	character_feet_input in;
	character_feet_output out;
	foot_ik_result result[2];

	b->feet_lift = 0.0f;
	memset(&in, 0, sizeof(in));
	in.want = feet_want(cm, style, x, y);
	if(in.want <= 0.0f && b->feet.weight <= 0.0f)
		return;

	in.x = x;
	in.y = y;
	in.floor_z = floor_z;
	in.yaw = b->motion.yaw;
	in.scale = body_scale(style);
	in.min_y = cm->model.min_y;
	in.rest_ankle[0] = cm->legs.rest_ankle[0];
	in.rest_ankle[1] = cm->legs.rest_ankle[1];
	character_legs_measure(&cm->model, &cm->legs, pose, &clip);
	character_feet_update(&b->feet, &clip, &in, dt, &out);
	if(!out.active)
		return;

	for(int leg = 0; leg < 2; leg++)
		foot_ik_solve_leg(&clip.leg[leg], out.raise[leg], FOOT_IK_MAX_REACH, &result[leg]);
	character_legs_apply(&cm->model, &cm->legs, &clip, result, out.tilt, pose);
	b->feet_lift = out.lift;
}

// The matrix that takes a body's rig to the world
static void body_model_matrix(const Body* b, const CharacterModel* cm, const CharacterStyle* style,
                              float x, float y, float z, mat4 out) {
	// World (x, y) at height z is GL (x, z, y); feet on the floor, scaled (D1-D3)
	glm_mat4_identity(out);
	glm_translate(out, (vec3){ x, z + style->lift + b->feet_lift, y });
	glm_rotate_y(out, b->motion.yaw, out);
	glm_scale_uni(out, body_scale(style));
	glm_translate(out, (vec3){ 0.0f, -cm->model.min_y, 0.0f });
}

// Swing the body's hair and tail in the pose it is about to be skinned in
static void apply_springs(Body* b, const CharacterModel* cm, const CharacterStyle* style,
                          float x, float y, float z, float dt, Pose* pose) {
	character_spring_params tail = { .stiffness = CHARACTER_TAIL_STIFFNESS,
	                                 .drag = CHARACTER_TAIL_DRAG,
	                                 .gravity = CHARACTER_TAIL_GRAVITY,
	                                 .max_speed = CHARACTER_HAIR_COLLIDE ? 0.0f : -1.0f };
	character_spring_params hair = { .stiffness = CHARACTER_HAIR_STIFFNESS,
	                                 .drag = CHARACTER_HAIR_DRAG,
	                                 .gravity = CHARACTER_HAIR_GRAVITY,
	                                 .max_speed = CHARACTER_HAIR_COLLIDE ? 0.0f : -1.0f };
	float dx = x - g_focus_x;
	float dy = y - g_focus_y;
	mat4 matrix;
	Mat4 to_world;

	if(cm->hair.count == 0 && cm->tail.count == 0)
		return;

	if(CHARACTER_HAIR_COLLIDE) {
		hair.drape_length = CHARACTER_HAIR_DRAPE_LENGTH;
		hair.drape_stiffness = CHARACTER_HAIR_DRAPE_STIFFNESS;
		hair.drape_gravity = CHARACTER_HAIR_DRAPE_GRAVITY;
		hair.drape_press = CHARACTER_HAIR_DRAPE_PRESS;
		hair.drape_reach = CHARACTER_HAIR_DRAPE_REACH;
	}

	if(CHARACTER_WIND) {
		const weather_now* weather = weather_now_get();

		wind_lean_at(weather, x, y, hair.wind);
		for(int i = 0; i < 3; i++)
			tail.wind[i] = hair.wind[i] * CHARACTER_TAIL_WIND_SHARE;
		hair.wind_flutter = tail.wind_flutter = CHARACTER_WIND_FLUTTER;
		hair.wind_time = tail.wind_time = weather->time_s;
	}

	hair.skip_colliders = dx * dx + dy * dy > CHARACTER_COLLIDE_RADIUS * CHARACTER_COLLIDE_RADIUS;
	body_model_matrix(b, cm, style, x, y, z, matrix);
	memcpy(to_world.m, matrix, sizeof(to_world.m));
	// The tail first: the hair keeps out of where the tail is this frame
	character_spring_update(&cm->model, &cm->tail, &tail, &to_world, dt, &b->tail, pose);
	character_spring_update(&cm->model, &cm->hair, &hair, &to_world, dt, &b->hair, pose);
}

/** Pose and skin a body, unless it is far away and was skinned recently. */
static void body_skin(Body* b, float x, float y, float z, const CharacterStyle* style) {
    float dx = x - g_focus_x, dy = y - g_focus_y;
    int far = dx * dx + dy * dy > CHARACTER_FULL_RATE_RADIUS * CHARACTER_FULL_RATE_RADIUS;
	float ik_dt;

    b->skin_wait += g_dt;
    if (far && b->skinned && b->skin_wait < 1.0f / CHARACTER_FAR_SKIN_HZ) return;
	ik_dt = b->skin_wait;
    b->skin_wait = 0.0f;

    const CharacterMotion* m = &b->motion;
	const CharacterModel* cm = &g_models[b->model];
	Pose* pose = &g_pose_a;

    sample_clip(b, m->anim, m->anim_time, &g_pose_a);
    if (m->blend < 1.0f) {
        sample_clip(b, m->prev, m->prev_time, &g_pose_b);
        skinned_model_blend(&KIT_MODEL, &g_pose_b, &g_pose_a, m->blend, &g_pose_a);
    }
	// The clips are the kit's; another model is posed from the kit's pose
	if(b->model != MODEL_KIT) {
		character_retarget_apply(&cm->retarget, &KIT_MODEL, &cm->model, &g_pose_a, &g_pose_t);
		pose = &g_pose_t;
	}
	character_pose_apply(&cm->model, &cm->rig, &m->proc.now, pose);
	apply_feet(b, cm, pose, x, y, z, style, ik_dt);
	apply_springs(b, cm, style, x, y, z, ik_dt, pose);
    skinned_model_skin(&cm->model, pose, &b->instance);
    b->skinned = 1;
}

void character_renderer_draw(CharacterKind kind, uint32_t id, float x, float y, float z,
                             const CharacterStyle* style) {
    if (!g_ready) 
        return;

    uint64_t key = ((uint64_t)kind << 32) | id;
    Body* b = body_for(key, x, y);
    if (!b) 
        return;

    b->last_seen = g_frame;
	body_wear(b, style);

    /* The clock runs whether or not the body is seen, so it does not jump
     * when it walks back into view; only the skinning and drawing are skipped. */
	// TEMPORARY DIAGNOSTIC: when a body's clip actually changes
	CharacterAnim anim_before = b->motion.anim;
	float durations[CHARACTER_ANIM_COUNT];

	// another emote clip in the same slot starts over, not mid-way
	if(style->hints.clip_custom && b->over[OVER_CUSTOM] != style->hints.clip_custom &&
	   over_index(b->motion.anim) == OVER_CUSTOM)
		b->motion.anim_time = 0.0f;
	b->over[OVER_WALK] = style->hints.clip_walk;
	b->over[OVER_JOG] = style->hints.clip_jog;
	b->over[OVER_SIT] = style->hints.clip_sit;
	// no emote clip any more: keep the last one, for the clip fading out to play on
	// (without it the fade shows the bind pose, a T-pose)
	if(style->hints.clip_custom)
		b->over[OVER_CUSTOM] = style->hints.clip_custom;
	body_durations(b, durations);

    character_motion_update(&b->motion, x, y, g_dt, &style->hints, durations);
	if(kind != CHARACTER_KIND_PREVIEW && b->motion.anim != anim_before)
		CLOG_INFO("[DIAG] body kind %d id %u anim %d -> %d (speed %.0f)", (int)kind, id,
		          (int)anim_before, (int)b->motion.anim, b->motion.speed);
    if (!body_on_screen(x, y, z + style->lift, CHARACTER_HEIGHT * style->scale)) return;
    body_skin(b, x, y, z, style);

	const CharacterModel* cm = &g_models[b->model];

    mat4 model, mvp;
	body_model_matrix(b, cm, style, x, y, z, model);
    glm_mat4_mul((vec4*)g_view.viewproj, model, mvp);

	// The next frame's shadow pass casts this body from where it stands now
	if(kind != CHARACTER_KIND_PREVIEW) {
		b->shadow_mark = 1;
		memcpy(b->shadow_model, model, sizeof(b->shadow_model));
	}

    model_renderer_draw(&cm->model, &b->instance, (const float*)mvp, (const float*)model,
                        style->tinted ? style->tint : NULL);
}

// The bodies the shadow pass casts this frame: the ones drawn since the last pass
static int g_shadow_bodies[CHARACTER_MAX_BODIES];
static int g_shadow_count;

void character_renderer_shadows_begin(void) {
	g_shadow_count = 0;
	for(int i = 0; i < CHARACTER_MAX_BODIES; i++) {
		Body* b = &g_bodies[i];

		if(b->key && b->shadow_mark)
			g_shadow_bodies[g_shadow_count++] = i;
		b->shadow_mark = 0;
	}
}

// Draw the collected bodies standing within `range` of (x, y)
static void draw_collected(const float* light_viewproj, float x, float y, float range) {
	if(!g_ready || g_shadow_count == 0)
		return;

	model_renderer_depth_begin();
	for(int n = 0; n < g_shadow_count; n++) {
		const Body* b = &g_bodies[g_shadow_bodies[n]];
		float dx = b->shadow_model[12] - x;
		float dy = b->shadow_model[14] - y;
		float reach = range + CHARACTER_HEIGHT;
		mat4 mvp;

		if(dx * dx + dy * dy > reach * reach)
			continue;
		glm_mat4_mul((vec4*)light_viewproj, (vec4*)b->shadow_model, mvp);
		model_renderer_draw_depth(&g_models[b->model].model, &b->instance, (const float*)mvp);
	}
	model_renderer_depth_end();
}

void character_renderer_draw_shadows_near(const float* light_viewproj, float x, float y,
                                          float range) {
	draw_collected(light_viewproj, x, y, range);
}

int character_renderer_local_fidgeting(void) {
	uint64_t key = ((uint64_t)CHARACTER_KIND_LOCAL << 32);

	for(int i = 0; i < CHARACTER_MAX_BODIES; i++)
		if(g_bodies[i].key == key)
			return g_bodies[i].motion.fidgeting ? 1 + g_bodies[i].motion.fidget_kind : 0;

	return 0;
}

void character_renderer_end(void) {
    if (!g_ready) 
        return;
    model_renderer_end();

    for (int i = 0; i < CHARACTER_MAX_BODIES; i++) {
        Body* b = &g_bodies[i];
        if (b->key && g_frame - b->last_seen > CHARACTER_FORGET_FRAMES)
            body_release(b);
    }
}
