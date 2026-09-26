/**
 * @file
 * Track the client camera, load it into OpenGL, and convert between screen and
 * world coordinates in either view.
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
    camera->mode = CAMERA_MODE_3D;

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

void camera_zoom_by(Camera* camera, float notches) {
    camera->zoom *= 1.0f + notches * CAMERA_ZOOM_STEP;
    if (camera->zoom < CAMERA_ZOOM_MIN) camera->zoom = CAMERA_ZOOM_MIN;
    if (camera->zoom > CAMERA_ZOOM_MAX) camera->zoom = CAMERA_ZOOM_MAX;
}

void camera_reset_view(Camera* camera) {
    camera->yaw  = deg_to_rad(CAMERA_YAW_DEFAULT_DEG);
    camera->zoom = CAMERA_ZOOM_DEFAULT;
}

void camera_toggle_mode(Camera* camera) {
    camera->mode = (camera->mode == CAMERA_MODE_3D) ? CAMERA_MODE_TOP_DOWN : CAMERA_MODE_3D;
    CLOG_INFO("[CAMERA] %s view", camera->mode == CAMERA_MODE_3D ? "3D" : "top-down");
}

void camera_get_view(const Camera* camera, CameraView* out) {
    if (camera->mode == CAMERA_MODE_TOP_DOWN) {
        camera_view_build_2d(out, camera->x, camera->y, camera->zoom,
                             camera->viewport_width, camera->viewport_height);
        return;
    }
    float fovy = deg_to_rad(CAMERA_FOVY_DEG);
    camera_view_build(out, camera->x, camera->y, camera->yaw, deg_to_rad(CAMERA_PITCH_DEG),
                      camera_math_distance_for_zoom(camera->zoom, camera->viewport_height, fovy),
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
 * In the 3D view this is the ground point under the cursor. A cursor above the
 * horizon cannot happen at the fixed pitch, but if it did the look-at point is
 * the least surprising answer.
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
    if (!g_applied_valid || g_applied.top_down) return;

    float sx = -10000.0f, sy = -10000.0f, depth = 1.0f;
    float scale = 0.0f;
    if (camera_view_project(&g_applied, anchor_x, anchor_y, lift, &sx, &sy, &depth))
        scale = camera_view_pixels_per_unit(&g_applied, anchor_x, anchor_y, lift);

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
    if (!g_applied_valid || g_applied.top_down) return;

    glDisable(GL_ALPHA_TEST);
    glDisable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);   /* the GL default, for whoever enables depth next */
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();
}

int camera_hit_billboard(const Camera* camera, float entity_x, float entity_y,
                         float foot_dy, float radius, float screen_x, float screen_y) {
    CameraView view;
    camera_get_view(camera, &view);

    if (view.top_down) {
        /* The body is drawn centred on the position, as it always was. */
        float wx, wy;
        if (!camera_view_pick_ground(&view, screen_x, screen_y, &wx, &wy)) return 0;
        float dx = wx - entity_x, dy = wy - entity_y;
        return dx * dx + dy * dy < radius * radius;
    }

    float sx, sy, depth;
    if (!camera_view_project(&view, entity_x, entity_y, 0.0f, &sx, &sy, &depth)) return 0;
    float scale = camera_view_pixels_per_unit(&view, entity_x, entity_y, 0.0f);
    float cx = sx, cy = sy - foot_dy * scale;   /* body centre, above the feet */
    float dx = screen_x - cx, dy = screen_y - cy;
    float r = radius * scale;
    return dx * dx + dy * dy < r * r;
}
