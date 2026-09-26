#ifndef CAMERA_MATH_H
#define CAMERA_MATH_H

/**
 * @file
 * Camera matrices, projection, and ground picking, with no OpenGL.
 *
 * The world the server simulates is a flat plane of (x, y). The client draws it
 * in 3D with Y up, so a world point (x, y) at height h sits at GL (x, h, y)
 * (decision D1 in Next_steps/3d_refactor.md). Both cameras -- the tilted,
 * rotating 3D one and the old top-down one -- are described by the same
 * CameraView, so projecting, picking and culling have one implementation.
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
    int   top_down;          /**< Nonzero for the orthographic top-down camera. */
} CameraView;

/** Build the tilted perspective camera orbiting a ground target.
 *
 * @param yaw       Rotation about the vertical axis, radians. 0 is north-up.
 * @param pitch     Angle below the horizon, radians.
 * @param distance  Eye distance from the target, world units.
 * @param fovy      Vertical field of view, radians.
 */
void camera_view_build(CameraView* out, float target_x, float target_y,
                       float yaw, float pitch, float distance, float fovy,
                       int viewport_w, int viewport_h);

/** Build the top-down orthographic camera the client has always used. */
void camera_view_build_2d(CameraView* out, float center_x, float center_y,
                          float zoom, int viewport_w, int viewport_h);

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

/** Eye distance at which a world unit at the target is `zoom` pixels tall. */
float camera_math_distance_for_zoom(float zoom, int viewport_h, float fovy);

#endif /* CAMERA_MATH_H */
