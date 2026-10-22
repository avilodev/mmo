#ifndef CHARACTER_RENDERER_H
#define CHARACTER_RENDERER_H

/**
 * @file
 * Draw players and NPCs as animated 3D characters.
 *
 * Three rigged models (the kit's mannequin for NPCs, and a male and a female
 * for players, which play the kit's clips: character_retarget.h), one body per
 * entity on screen. A body is looked up
 * by a key built from what the entity is (local player, remote player, NPC) and
 * its server id, so its animation carries over from frame to frame without the
 * game state having to hold anything render-side. Bodies nobody draws for a
 * while are reclaimed.
 *
 * Positions stay 2D world (x, y): a body stands on the ground at GL (x, 0, y).
 */

#include <stdint.h>

#include "camera/camera_math.h"
#include "render/character_motion.h"

/** What kind of entity a body belongs to; with its id, the body's key. */
typedef enum {
    CHARACTER_KIND_LOCAL = 1,
    CHARACTER_KIND_PLAYER,
    CHARACTER_KIND_NPC,
	CHARACTER_KIND_PREVIEW  /**< A body in an emote hub box; id is the box. */
} CharacterKind;

/** How one entity should look this frame. */
typedef struct {
    float tint[3];          /**< Body colour, worn only when `tinted` is set. */
    float scale;            /**< 1.0 is CHARACTER_HEIGHT tall. */
    float lift;             /**< Height of the feet above the ground (a jump). */
    CharacterHints hints;

	/** The race's stable key ("wolf"), which picks its ears and tail from the
	 *  model; NULL for an NPC, which wears a body and no race parts. */
	const char* race_key;
	uint32_t appearance;    /**< Packed look; only the body is read (character_look.h). */

	/** Nonzero tints the body with `tint`; zero draws it as authored. */
	int tinted;
} CharacterStyle;

/** Load the model and its shader. Needs gl_loader_init().
 *  @return Nonzero when characters can be drawn; failures are logged, and
 *          character_renderer_draw() then draws nothing. */
int  character_renderer_init(void);
void character_renderer_shutdown(void);

// Whether bodies wear their animal features (ears, tails and the like); off draws
// everyone as a human. Bodies dress again on their next draw, so it can change at any time.
void character_renderer_set_animal_features(int show);

// Drop every body it holds, so the next ones start clean. The model stays loaded.
void character_renderer_reset(void);

// How many bodies it is holding now
int character_renderer_body_count(void);

/** Whether the model loaded, so callers can keep a fallback for when it did not. */
int  character_renderer_ready(void);

/** Start a frame of character draws under a camera.
 *
 * @param focus_x, focus_y  Where the camera looks; bodies far from it animate
 *                          at a reduced rate.
 * @param dt                Seconds since the last frame.
 */
void character_renderer_begin(const CameraView* view, float focus_x, float focus_y, float dt);

/** Where the camera centres this frame, for the label range. Set before the
 *  ground pass; character_renderer_begin() sets it too. */
void character_renderer_set_focus(float x, float y);

/** Whether a ground point is near enough to the camera's centre for a name
 *  plate or health bar (WORLD_LABEL_RANGE, world_light.h). */
int character_renderer_in_label_range(float x, float y);

/** A soft shadow on the floor under a body of `scale` at height z, so it
 *  stands on the floor rather than floating over it. The shadow lies on the
 *  highest floor at or below the body, so a jump leaves it behind on the
 *  surface and it shrinks with the gap. Draw in the ground pass (camera
 *  applied, before structures); it is plain fixed-function drawing. */
void character_draw_shadow(float x, float y, float z, float scale);

/** How long a clip plays, in seconds; 0 before the model loads or when the
 *  clip is missing. */
float character_renderer_clip_seconds(CharacterAnim anim);

// How long an emote clip (an index in the emote clip table) runs, or 0
float character_renderer_table_clip_seconds(int clip);

/** Draw one entity's body standing at world (x, y) on a floor at height z. */
void character_renderer_draw(CharacterKind kind, uint32_t id, float x, float y, float z,
                             const CharacterStyle* style);

// Shadows: bodies cast from where they were drawn last frame, a frame behind
// and unnoticeably so. shadows_begin() takes the bodies drawn since its last
// call; draw_shadows_near() then draws those within `range` of the ground
// point (x, y) into the shadow map being drawn (render/shadow_map.h).
void character_renderer_shadows_begin(void);
void character_renderer_draw_shadows_near(const float* light_viewproj, float x, float y,
                                          float range);

/** Finish the frame: restore GL state and reclaim bodies not drawn lately. */
void character_renderer_end(void);

/** 0 when the local body is not playing an idle variation, else 1 + which one. */
int character_renderer_local_fidgeting(void);

#endif /* CHARACTER_RENDERER_H */
