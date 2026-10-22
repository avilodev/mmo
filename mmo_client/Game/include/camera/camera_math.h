#ifndef CAMERA_MATH_H
#define CAMERA_MATH_H

/**
 * @file
 * Camera matrices, projection, and ground picking, with no OpenGL.
 *
 * The world the server simulates is a flat plane of (x, y). The client draws it
 * in 3D with Y up, so a world point (x, y) at height h sits at GL (x, h, y)
 * (decision D1 in Next_steps/3d_refactor.md).
 *
 * Kept free of GL so it can be unit-tested headless (D27).
 */

/** One frame's camera: every matrix needed to draw and to pick. Column-major. */
typedef struct {
    float view[16];          /**< World (GL, Y up) to eye. */
    float proj[16];          /**< Eye to clip. */
    float viewproj[16];      /**< proj * view. */
    float inv_viewproj[16];  /**< Clip back to world, for picking. */
    float ground_view[16];   /**< view * ground: takes (x, y, h) vertices directly. */
    float ground_mvp[16];    /**< proj * view * ground. */
    int   viewport_w;        /**< Logical viewport the screen coordinates refer to. */
    int   viewport_h;
} CameraView;

/** Build the perspective camera orbiting a target point.
 *
 * @param target_h  Height of the point looked at: the ground (0) or, to frame
 *                  a character, part-way up their body.
 * @param yaw       Rotation about the vertical axis, radians. 0 is north-up.
 * @param pitch     Angle below the horizon, radians.
 * @param distance  Eye distance from the target, world units.
 * @param fovy      Vertical field of view, radians.
 */
void camera_view_build(CameraView* out, float target_x, float target_y, float target_h,
                       float yaw, float pitch, float distance, float fovy,
                       int viewport_w, int viewport_h);

/** Project a world point at a height to logical screen pixels (y down).
 *
 * @param depth_ndc  Normalized depth in [-1, 1]; smaller is nearer.
 * @return Nonzero when the point is in front of the camera.
 */
int camera_view_project(const CameraView* v, float wx, float wy, float height,
                        float* sx, float* sy, float* depth_ndc);

/** Intersect the ray under a screen point with the ground.
 *
 * @return Nonzero when the ray hits the ground in front of the camera.
 */
int camera_view_pick_ground(const CameraView* v, float sx, float sy,
                            float* wx, float* wy);

/** The ray under a screen point, in world coordinates (x, y, z up): `origin` is
 *  on the near plane and `dir` is a unit vector away from the eye. It is the
 *  ray camera_view_pick_ground() intersects with the ground. */
void camera_view_screen_ray(const CameraView* v, float sx, float sy,
                            float origin[3], float dir[3]);

/** Where the eye is, in GL space (x, up, z). */
void camera_view_eye(const CameraView* v, float eye[3]);

/** The axis-aligned world rectangle containing all ground on screen. */
void camera_view_ground_bounds(const CameraView* v, float* min_x, float* min_y,
                               float* max_x, float* max_y);

/** Screen pixels per world unit for something standing at a world point. */
float camera_view_pixels_per_unit(const CameraView* v, float wx, float wy, float height);

/** Turn screen-relative movement input into a world direction.
 *
 * (ix, iy) is in screen terms: +x right, +y down, so W is (0, -1). The result
 * is the world direction that moves the same way on screen at this yaw, with
 * the same length as the input.
 */
void camera_math_rotate_input(float yaw, float ix, float iy, float* wx, float* wy);

/** The orbit distance actually used: `distance`, shortened when the eye
 *  would otherwise sit lower than `min_eye_h` above the floor -- a camera
 *  tilted below the target slides in along the floor rather than through it.
 *  `floor_z` is the height of that floor (0 on level ground); `target_h` is
 *  an absolute height, so it includes the floor the target stands on. */
float camera_math_floor_distance(float pitch, float target_h, float distance, float min_eye_h,
                                 float floor_z);

/** Height of whatever is solid at a world point, in world units; 0 where
 *  nothing is. `ctx` is whatever the caller registered with it. */
typedef float (*CameraHeightFn)(void* ctx, float world_x, float world_y);

/** Whether a world point at height `h` is free walkable air: above a floor
 *  and under its ceiling. The height source is flat -- one top per tile -- so
 *  a stair tile beside a wall reads as solid; air the player can stand in is
 *  not inside that wall. `ctx` is the caller's, as for CameraHeightFn. */
typedef int (*camera_open_fn)(void* ctx, float world_x, float world_y, float h);

/** The orbit distance the eye can reach before it would sit inside something
 *  solid: `distance`, shortened to just short of the first point along the
 *  line from the target to the eye (by `margin`) that is lower than the solid
 *  there and is not free walkable air (`open_at`, NULL for none). The line is
 *  sampled every `step` units; `distance` is returned unchanged when nothing
 *  is in the way or `height_at` is NULL. */
float camera_math_clear_distance(float target_x, float target_y, float target_h,
                                 float yaw, float pitch, float distance,
                                 float step, float margin, CameraHeightFn height_at,
                                 camera_open_fn open_at, void* ctx);

/** Eye distance at which a world unit at the target is `zoom` pixels tall. */
float camera_math_distance_for_zoom(float zoom, int viewport_h, float fovy);

#endif /* CAMERA_MATH_H */
