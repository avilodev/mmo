#ifndef CHARACTER_RENDERER_H
#define CHARACTER_RENDERER_H

/**
 * @file
 * Draw players and NPCs as animated 3D characters.
 *
 * One shared rigged model, one body per entity on screen. A body is looked up
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
    CHARACTER_KIND_NPC
} CharacterKind;

/** How one entity should look this frame. */
typedef struct {
    float tint[3];          /**< Body colour. */
    float scale;            /**< 1.0 is CHARACTER_HEIGHT tall. */
    float lift;             /**< Height of the feet above the ground (a jump). */
    CharacterHints hints;
} CharacterStyle;

/** Load the model and its shader. Needs gl_loader_init().
 *  @return Nonzero when characters can be drawn; failures are logged, and
 *          character_renderer_draw() then draws nothing. */
int  character_renderer_init(void);
void character_renderer_shutdown(void);

/** Whether the model loaded, so callers can keep a fallback for when it did not. */
int  character_renderer_ready(void);

/** Start a frame of character draws under a camera.
 *
 * @param focus_x, focus_y  Where the camera looks; bodies far from it animate
 *                          at a reduced rate.
 * @param dt                Seconds since the last frame.
 */
void character_renderer_begin(const CameraView* view, float focus_x, float focus_y, float dt);

/** Where the camera centres this frame, for the fog. Set before the ground
 *  pass; character_renderer_begin() sets it too. */
void character_renderer_set_focus(float x, float y);

/** How deep in the fog a ground point is: 0 clear, 1 gone (world_light.h). */
float character_renderer_fog_at(float x, float y);

/** A soft shadow on the ground under a body of `scale`, so it stands on the
 *  floor rather than floating over it. Draw in the ground pass (camera
 *  applied, before structures); it is plain fixed-function drawing. */
void character_draw_shadow(float x, float y, float scale);

/** Draw one entity's body standing at world (x, y). */
void character_renderer_draw(CharacterKind kind, uint32_t id, float x, float y,
                             const CharacterStyle* style);

/** Finish the frame: restore GL state and reclaim bodies not drawn lately. */
void character_renderer_end(void);

#endif /* CHARACTER_RENDERER_H */
