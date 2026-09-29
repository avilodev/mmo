/**
 * @file
 * Track the client camera, load it into OpenGL, and convert between screen and
 * world coordinates.
 */

#include "camera.h"
#include "camera/camera_tuning.h"
#include <GLFW/glfw3.h>
#include <math.h>
#include <stdio.h>
#include "core/client_log.h"

#define TWO_PI 6.28318530718f

static float deg_to_rad(float deg) { return deg * 3.14159265359f / 180.0f; }

/** The view camera_apply() last loaded, for billboards drawn this frame. */
static CameraView g_applied;
static int        g_applied_valid = 0;

/**
 * Initialize a camera for a logical viewport.
 *
 * @param viewport_width  Logical viewport width in pixels.
 * @param viewport_height  Logical viewport height in pixels.
 */
void camera_init(Camera* camera, int viewport_width, int viewport_height) {
    camera->x = 0.0f;
    camera->y = 0.0f;
    camera->target_x = 0.0f;
    camera->target_y = 0.0f;
    camera->smoothing = 0.05f;  // Adjust this for smoother/snappier camera (0.05 = very smooth, 0.2 = snappy)
    camera->viewport_width = viewport_width;
    camera->viewport_height = viewport_height;
    camera->zoom = CAMERA_ZOOM_DEFAULT;
    camera->yaw = deg_to_rad(CAMERA_YAW_DEFAULT_DEG);
    camera->pitch = deg_to_rad(CAMERA_PITCH_DEFAULT_DEG);

    CLOG_INFO("Camera initialized: viewport %dx%d", viewport_width, viewport_height);
}

/**
 * Move the camera toward a target using frame-rate-adjusted interpolation.
 *
 * @param delta_time  Elapsed frame time in seconds.
 */
void camera_update(Camera* camera, float target_x, float target_y, double delta_time) {
    camera->target_x = target_x;
    camera->target_y = target_y;

    float lerp_factor = 1.0f - powf(camera->smoothing, (float)(delta_time * 60.0));
    camera->x += (target_x - camera->x) * lerp_factor;
    camera->y += (target_y - camera->y) * lerp_factor;
}

/**
 * Snap the camera and its interpolation target to a world position.
 */
void camera_set_position(Camera* camera, float x, float y) {
    camera->x = x;
    camera->y = y;
    camera->target_x = x;
    camera->target_y = y;
}

void camera_rotate(Camera* camera, float delta_radians) {
    camera->yaw = fmodf(camera->yaw + delta_radians, TWO_PI);
    if (camera->yaw < 0.0f) camera->yaw += TWO_PI;
}

void camera_pitch_by(Camera* camera, float delta_radians) {
    float lo = deg_to_rad(CAMERA_PITCH_MIN_DEG), hi = deg_to_rad(CAMERA_PITCH_MAX_DEG);
    camera->pitch += delta_radians;
    if (camera->pitch < lo) camera->pitch = lo;
    if (camera->pitch > hi) camera->pitch = hi;
}

void camera_zoom_by(Camera* camera, float notches) {
    camera->zoom *= 1.0f + notches * CAMERA_ZOOM_STEP;
    if (camera->zoom < CAMERA_ZOOM_MIN) camera->zoom = CAMERA_ZOOM_MIN;
    if (camera->zoom > CAMERA_ZOOM_MAX) camera->zoom = CAMERA_ZOOM_MAX;
}

void camera_reset_view(Camera* camera) {
    camera->yaw   = deg_to_rad(CAMERA_YAW_DEFAULT_DEG);
    camera->pitch = deg_to_rad(CAMERA_PITCH_DEFAULT_DEG);
    camera->zoom  = CAMERA_ZOOM_DEFAULT;
}

void camera_get_view(const Camera* camera, CameraView* out) {
    float fovy = deg_to_rad(CAMERA_FOVY_DEG);
    float distance = camera_math_distance_for_zoom(camera->zoom, camera->viewport_height, fovy);
    distance = camera_math_floor_distance(camera->pitch, CAMERA_TARGET_HEIGHT, distance,
                                          CAMERA_MIN_EYE_HEIGHT);
    camera_view_build(out, camera->x, camera->y, CAMERA_TARGET_HEIGHT, camera->yaw, camera->pitch,
                      distance,
                      fovy, camera->viewport_width, camera->viewport_height);
}

/**
 * Apply the camera as the current OpenGL projection.
 *
 * A current OpenGL context must exist before this call.
 */
void camera_apply(const Camera* camera) {
    camera_get_view(camera, &g_applied);
    g_applied_valid = 1;

    glMatrixMode(GL_PROJECTION);
    glLoadMatrixf(g_applied.proj);
    glMatrixMode(GL_MODELVIEW);
    glLoadMatrixf(g_applied.ground_view);
}

/**
 * Convert logical screen coordinates to world coordinates.
 *
 * The ground point under the cursor. A cursor above the horizon (a low camera
 * looking out) has no ground under it; the look-at point is the least
 * surprising answer.
 *
 * @param world_x  Destination for the converted world X coordinate.
 * @param world_y  Destination for the converted world Y coordinate.
 */
void camera_screen_to_world(const Camera* camera, float screen_x, float screen_y,
                            float* world_x, float* world_y) {
    CameraView view;
    camera_get_view(camera, &view);
    if (!camera_view_pick_ground(&view, screen_x, screen_y, world_x, world_y)) {
        *world_x = camera->x;
        *world_y = camera->y;
    }
}

/**
 * Convert world coordinates to logical screen coordinates.
 *
 * @param screen_x  Destination for the converted screen X coordinate.
 * @param screen_y  Destination for the converted screen Y coordinate.
 */
void camera_world_to_screen(const Camera* camera, float world_x, float world_y,
                            float* screen_x, float* screen_y) {
    CameraView view;
    float depth;
    camera_get_view(camera, &view);
    if (!camera_view_project(&view, world_x, world_y, 0.0f, screen_x, screen_y, &depth)) {
        *screen_x = -1.0f;
        *screen_y = -1.0f;
    }
}

void camera_visible_ground(const Camera* camera, float* min_x, float* min_y,
                           float* max_x, float* max_y) {
    CameraView view;
    camera_get_view(camera, &view);
    camera_view_ground_bounds(&view, min_x, min_y, max_x, max_y);
}

void camera_billboard_begin(float anchor_x, float anchor_y, float foot_dy, float lift) {
    if (!g_applied_valid) return;

    float sx = -10000.0f, sy = -10000.0f, depth = 1.0f;
    float scale = 0.0f;
    if (camera_view_project(&g_applied, anchor_x, anchor_y, lift, &sx, &sy, &depth))
        scale = camera_view_pixels_per_unit(&g_applied, anchor_x, anchor_y, lift);
    /* Cards shrink with distance but stop growing up close: a name plate
     * over someone beside a close camera stays a name plate, not a banner. */
    if (scale > CAMERA_CARD_MAX_SCALE) scale = CAMERA_CARD_MAX_SCALE;

    /* Screen space, with the card at the anchor's depth. glOrtho's -1..1 range
     * maps eye z to NDC z negated, hence -depth. */
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glOrtho(0, g_applied.viewport_w, g_applied.viewport_h, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();
    glTranslatef(sx, sy, -depth);
    glScalef(scale, scale, 1.0f);
    glTranslatef(-anchor_x, -(anchor_y + foot_dy), 0.0f);

    /* One card hides the cards behind it; its transparent texels do not. */
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glEnable(GL_ALPHA_TEST);
    glAlphaFunc(GL_GREATER, 0.02f);
}

void camera_billboard_end(void) {
    if (!g_applied_valid) return;

    glDisable(GL_ALPHA_TEST);
    glDisable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);   /* the GL default, for whoever enables depth next */
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();
}

int camera_hit_standing(const Camera* camera, float entity_x, float entity_y,
                        float height, float radius, float screen_x, float screen_y) {
    CameraView view;
    camera_get_view(camera, &view);

    float fx, fy, hx, hy, depth;
    if (!camera_view_project(&view, entity_x, entity_y, 0.0f, &fx, &fy, &depth)) return 0;
    if (!camera_view_project(&view, entity_x, entity_y, height, &hx, &hy, &depth)) return 0;
    float r = radius * camera_view_pixels_per_unit(&view, entity_x, entity_y, height * 0.5f);

    /* Distance from the cursor to the projected feet-to-head segment. */
    float sx = hx - fx, sy = hy - fy;
    float len2 = sx * sx + sy * sy;
    float t = (len2 > 1e-6f) ? ((screen_x - fx) * sx + (screen_y - fy) * sy) / len2 : 0.0f;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    float dx = screen_x - (fx + t * sx), dy = screen_y - (fy + t * sy);
    return dx * dx + dy * dy < r * r;
}
