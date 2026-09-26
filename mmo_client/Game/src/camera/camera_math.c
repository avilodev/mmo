/**
 * @file
 * Build camera matrices and project, pick and cull against the ground plane.
 *
 * World (x, y) at height h is GL (x, h, y), Y up. The "ground" matrix does that
 * swap, so everything the client has always drawn with glVertex2f(x, y) lands
 * on the floor unchanged, and a third coordinate becomes height.
 */

#include "camera/camera_math.h"
#include "camera/cglm_config.h"

#include <math.h>
#include <string.h>

/** Eye height of the top-down camera; anything above the ground works. */
#define TOP_DOWN_EYE_HEIGHT 1000.0f

/** Map (x, y, h) to GL (x, h, y). */
static void ground_matrix(mat4 g) {
    glm_mat4_identity(g);
    g[1][1] = 0.0f; g[1][2] = 1.0f;   /* y -> Z */
    g[2][1] = 1.0f; g[2][2] = 0.0f;   /* h -> Y */
}

/** Derive every product and inverse from view and proj. */
static void finish(CameraView* out, mat4 view, mat4 proj) {
    mat4 viewproj, inv, g, gv, gmvp;
    glm_mat4_mul(proj, view, viewproj);
    glm_mat4_inv(viewproj, inv);
    ground_matrix(g);
    glm_mat4_mul(view, g, gv);
    glm_mat4_mul(proj, gv, gmvp);

    memcpy(out->view,         view,     sizeof(out->view));
    memcpy(out->proj,         proj,     sizeof(out->proj));
    memcpy(out->viewproj,     viewproj, sizeof(out->viewproj));
    memcpy(out->inv_viewproj, inv,      sizeof(out->inv_viewproj));
    memcpy(out->ground_view,  gv,       sizeof(out->ground_view));
    memcpy(out->ground_mvp,   gmvp,     sizeof(out->ground_mvp));
}

void camera_view_build(CameraView* out, float target_x, float target_y,
                       float yaw, float pitch, float distance, float fovy,
                       int viewport_w, int viewport_h) {
    vec3 target = { target_x, 0.0f, target_y };
    vec3 eye = {
        target_x + distance * sinf(yaw) * cosf(pitch),
        distance * sinf(pitch),
        target_y + distance * cosf(yaw) * cosf(pitch)
    };
    vec3 up = { 0.0f, 1.0f, 0.0f };

    mat4 view, proj;
    glm_lookat(eye, target, up, view);

    float aspect = (viewport_h > 0) ? (float)viewport_w / (float)viewport_h : 1.0f;
    /* Near and far scale with distance so depth precision follows the zoom.
     * Everything drawn sits within a few distances of the target. */
    glm_perspective(fovy, aspect, distance * 0.05f, distance * 8.0f, proj);

    out->viewport_w = viewport_w;
    out->viewport_h = viewport_h;
    out->top_down   = 0;
    finish(out, view, proj);
}

void camera_view_build_2d(CameraView* out, float center_x, float center_y,
                          float zoom, int viewport_w, int viewport_h) {
    if (zoom <= 0.0f) zoom = 1.0f;

    vec3 target = { center_x, 0.0f, center_y };
    vec3 eye    = { center_x, TOP_DOWN_EYE_HEIGHT, center_y };
    vec3 up     = { 0.0f, 0.0f, -1.0f };   /* north is up the screen */

    mat4 view, proj;
    glm_lookat(eye, target, up, view);

    float hw = (float)viewport_w / (2.0f * zoom);
    float hh = (float)viewport_h / (2.0f * zoom);
    glm_ortho(-hw, hw, -hh, hh, 1.0f, TOP_DOWN_EYE_HEIGHT * 2.0f, proj);

    out->viewport_w = viewport_w;
    out->viewport_h = viewport_h;
    out->top_down   = 1;
    finish(out, view, proj);
}

int camera_view_project(const CameraView* v, float wx, float wy, float height,
                        float* sx, float* sy, float* depth_ndc) {
    vec4 p = { wx, height, wy, 1.0f };
    vec4 clip;
    glm_mat4_mulv((vec4*)v->viewproj, p, clip);
    if (clip[3] <= 1e-6f) return 0;

    float nx = clip[0] / clip[3];
    float ny = clip[1] / clip[3];
    float nz = clip[2] / clip[3];

    *sx = (nx + 1.0f) * 0.5f * (float)v->viewport_w;
    *sy = (1.0f - ny) * 0.5f * (float)v->viewport_h;
    *depth_ndc = nz;
    return 1;
}

/** Unproject a screen point at an NDC depth to a GL world point. */
static void unproject(const CameraView* v, float sx, float sy, float ndc_z, vec3 out) {
    float nx = sx / (float)v->viewport_w * 2.0f - 1.0f;
    float ny = 1.0f - sy / (float)v->viewport_h * 2.0f;
    vec4 clip = { nx, ny, ndc_z, 1.0f };
    vec4 world;
    glm_mat4_mulv((vec4*)v->inv_viewproj, clip, world);
    float w = (fabsf(world[3]) > 1e-9f) ? world[3] : 1e-9f;
    out[0] = world[0] / w;
    out[1] = world[1] / w;
    out[2] = world[2] / w;
}

int camera_view_pick_ground(const CameraView* v, float sx, float sy,
                            float* wx, float* wy) {
    vec3 near_pt, far_pt;
    unproject(v, sx, sy, -1.0f, near_pt);
    unproject(v, sx, sy,  1.0f, far_pt);

    float dy = far_pt[1] - near_pt[1];
    if (fabsf(dy) < 1e-6f) return 0;          /* ray parallel to the ground */

    float t = -near_pt[1] / dy;
    if (t < 0.0f) return 0;                    /* ground is behind the eye */

    *wx = near_pt[0] + t * (far_pt[0] - near_pt[0]);
    *wy = near_pt[2] + t * (far_pt[2] - near_pt[2]);
    return 1;
}

void camera_view_ground_bounds(const CameraView* v, float* min_x, float* min_y,
                               float* max_x, float* max_y) {
    const float corners[4][2] = {
        { 0.0f, 0.0f },
        { (float)v->viewport_w, 0.0f },
        { 0.0f, (float)v->viewport_h },
        { (float)v->viewport_w, (float)v->viewport_h },
    };

    *min_x = *min_y =  INFINITY;
    *max_x = *max_y = -INFINITY;

    for (int i = 0; i < 4; i++) {
        float wx, wy;
        if (!camera_view_pick_ground(v, corners[i][0], corners[i][1], &wx, &wy)) {
            /* A corner above the horizon sees no ground; the far plane is the
             * furthest anything can be drawn, so bound it there instead. */
            vec3 far_pt;
            unproject(v, corners[i][0], corners[i][1], 1.0f, far_pt);
            wx = far_pt[0];
            wy = far_pt[2];
        }
        if (wx < *min_x) *min_x = wx;
        if (wx > *max_x) *max_x = wx;
        if (wy < *min_y) *min_y = wy;
        if (wy > *max_y) *max_y = wy;
    }
}

float camera_view_pixels_per_unit(const CameraView* v, float wx, float wy, float height) {
    /* The camera's right axis is row 0 of the view matrix, in GL (x, h, y). */
    float rx = v->view[0];
    float rh = v->view[4];
    float ry = v->view[8];

    float ax, ay, bx, by, depth;
    if (!camera_view_project(v, wx, wy, height, &ax, &ay, &depth)) return 0.0f;
    if (!camera_view_project(v, wx + rx, wy + ry, height + rh, &bx, &by, &depth)) return 0.0f;
    return sqrtf((bx - ax) * (bx - ax) + (by - ay) * (by - ay));
}

void camera_math_rotate_input(float yaw, float ix, float iy, float* wx, float* wy) {
    /* On the ground, the camera's right is (cos, -sin) and its forward (away
     * from the eye) is (-sin, -cos); input y is screen-down, so forward is -iy. */
    float c = cosf(yaw), s = sinf(yaw);
    *wx =  ix * c + iy * s;
    *wy = -ix * s + iy * c;
}

float camera_math_distance_for_zoom(float zoom, int viewport_h, float fovy) {
    if (zoom <= 0.0f) zoom = 1.0f;
    return (float)viewport_h / (2.0f * zoom * tanf(fovy * 0.5f));
}
