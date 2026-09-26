#ifndef CAMERA_H
#define CAMERA_H

/**
 * @file
 * The game camera: a tilted 3D view the player can rotate around their
 * character, plus the old top-down view behind a development toggle.
 *
 * Positions stay 2D world (x, y) everywhere outside this module. The camera
 * decides how that flat world is shown; the server never hears about it.
 * See Next_steps/3d_refactor.md for the decisions behind every number here,
 * and camera_tuning.h for the numbers themselves.
 */

#include "camera/camera_math.h"

/** How the world is viewed. */
typedef enum {
    CAMERA_MODE_3D = 0,     /**< Tilted, rotatable perspective (the game). */
    CAMERA_MODE_TOP_DOWN    /**< The original orthographic view (F9, development). */
} CameraMode;

/** Track a smoothed follow target, zoom, rotation and viewport. */
typedef struct {
    float x;            /**< Current look-at point, world units. */
    float y;
    float target_x;
    float target_y;
    float smoothing;    /**< Interpolation factor from instant at 0.0 to stationary at 1.0. */
    int viewport_width;
    int viewport_height;
    float zoom;         /**< Pixel scale at the look-at point (camera_tuning.h limits). */
    float yaw;          /**< Rotation about the vertical axis, radians; 0 is north-up. */
    CameraMode mode;
} Camera;

void camera_init(Camera* camera, int viewport_width, int viewport_height);

void camera_update(Camera* camera, float target_x, float target_y, double delta_time);

void camera_set_position(Camera* camera, float x, float y);

/** Turn the camera about the player.
 *
 * Positive moves the eye counter-clockwise around the player seen from above,
 * so the world on screen appears to turn clockwise. E is positive, Q negative.
 */
void camera_rotate(Camera* camera, float delta_radians);

/** Zoom by scroll notches (positive zooms in), clamped to the tuning limits. */
void camera_zoom_by(Camera* camera, float notches);

/** Back to north-up at the default zoom. */
void camera_reset_view(Camera* camera);

/** Switch between the 3D and top-down views. */
void camera_toggle_mode(Camera* camera);

/** This frame's matrices for the camera as it stands. */
void camera_get_view(const Camera* camera, CameraView* out);

/** Load the camera into OpenGL for the world pass.
 *
 * World-space drawing after this lands on the ground: glVertex2f(x, y) is the
 * ground point (x, y), and a third coordinate is height. Also remembers this
 * view for camera_billboard_begin(). A current OpenGL context must exist.
 */
void camera_apply(const Camera* camera);

void camera_screen_to_world(const Camera* camera, float screen_x, float screen_y,
                            float* world_x, float* world_y);

void camera_world_to_screen(const Camera* camera, float world_x, float world_y,
                            float* screen_x, float* screen_y);

/** The world rectangle containing every ground point on screen. */
void camera_visible_ground(const Camera* camera, float* min_x, float* min_y,
                           float* max_x, float* max_y);

/** Stand a flat 2D drawing up at a ground point, facing the camera.
 *
 * Everything drawn until camera_billboard_end() is in the same 2D world
 * coordinates the top-down view used, but shown upright: screen-aligned, at the
 * perspective scale of the anchor, and depth-tested against other billboards as
 * one card. The 2D point (anchor_x, anchor_y + foot_dy) -- the drawing's feet --
 * lands on the ground at (anchor_x, anchor_y). `lift` raises the whole card
 * above the ground, for things that fly.
 *
 * In the top-down view this does nothing, so the drawing is exactly what it
 * always was. Uses the view from the last camera_apply().
 */
void camera_billboard_begin(float anchor_x, float anchor_y, float foot_dy, float lift);
void camera_billboard_end(void);

/** Whether a screen point is over an entity drawn with camera_billboard_begin.
 *
 * @param foot_dy  As passed to camera_billboard_begin: the body's centre is
 *                 foot_dy above its feet.
 * @param radius   Hit radius around the body centre, in world units.
 */
int camera_hit_billboard(const Camera* camera, float entity_x, float entity_y,
                         float foot_dy, float radius, float screen_x, float screen_y);

#endif // CAMERA_H
