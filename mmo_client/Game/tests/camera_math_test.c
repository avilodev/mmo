/**
 * @file
 * Check the camera math the 3D view is built on, without an OpenGL context.
 *
 * Every mistake in here is a silent visual bug in the game: a click that lands
 * a tile away from the cursor, a chunk missing at the edge of the screen, W
 * walking sideways after the camera turns. So the properties are pinned
 * directly -- what projects where, that picking undoes projecting, that the
 * movement keys agree with what the screen shows -- at several yaws and both
 * zoom limits, rather than at the one angle that happens to be easy.
 */

#include "camera/camera_math.h"
#include "camera/camera_tuning.h"
#include "world/world.h"

#include <math.h>
#include <stdio.h>

static int g_failures = 0;

#define CHECK(cond, what)                                                   \
    do {                                                                    \
        if (cond) {                                                         \
            printf("  ok   %s\n", (what));                                  \
        } else {                                                            \
            printf("  FAIL %s  (%s:%d)\n", (what), __FILE__, __LINE__);     \
            g_failures++;                                                   \
        }                                                                   \
    } while (0)

#define VW 1920
#define VH 1080
/* The runtime tile size (game.c passes 16 to world_init). */
#define TILE 16.0f

static const float YAWS_DEG[] = { 0.0f, 45.0f, 90.0f, 180.0f, 270.0f, 333.0f };
#define YAW_COUNT ((int)(sizeof(YAWS_DEG) / sizeof(YAWS_DEG[0])))

static float rad(float deg) { return deg * 3.14159265f / 180.0f; }

static void build_3d(CameraView* v, float tx, float ty, float yaw_deg, float zoom) {
    camera_view_build(v, tx, ty, 0.0f, rad(yaw_deg), rad(CAMERA_PITCH_DEFAULT_DEG),
                      camera_math_distance_for_zoom(zoom, VH, rad(CAMERA_FOVY_DEG)),
                      rad(CAMERA_FOVY_DEG), VW, VH);
}

int main(void) {
    printf("=== camera math ===\n");

    printf("\nTEST 1: the target is always the centre of the screen\n");
    {
        int all = 1;
        for (int i = 0; i < YAW_COUNT; i++) {
            for (float zoom = CAMERA_ZOOM_MIN; zoom <= CAMERA_ZOOM_MAX; zoom += 0.5f) {
                CameraView v;
                build_3d(&v, 5000.0f, 3000.0f, YAWS_DEG[i], zoom);
                float sx, sy, depth;
                int ok = camera_view_project(&v, 5000.0f, 3000.0f, 0.0f, &sx, &sy, &depth);
                if (!ok || fabsf(sx - VW / 2.0f) > 0.5f || fabsf(sy - VH / 2.0f) > 0.5f)
                    all = 0;
            }
        }
        CHECK(all, "the player sits mid-screen at every yaw and zoom");
    }

    printf("\nTEST 2: picking undoes projecting\n");
    {
        int screen_ok = 1, world_ok = 1;
        for (int i = 0; i < YAW_COUNT; i++) {
            for (int z = 0; z < 2; z++) {
                float zoom = z ? CAMERA_ZOOM_MAX : CAMERA_ZOOM_MIN;
                CameraView v;
                build_3d(&v, 2048.0f, 1024.0f, YAWS_DEG[i], zoom);
                for (float sx = 40.0f; sx < VW; sx += 180.0f) {
                    for (float sy = 40.0f; sy < VH; sy += 140.0f) {
                        float wx, wy, bx, by, depth;
                        if (!camera_view_pick_ground(&v, sx, sy, &wx, &wy)) { screen_ok = 0; continue; }
                        if (!camera_view_project(&v, wx, wy, 0.0f, &bx, &by, &depth)) { screen_ok = 0; continue; }
                        if (fabsf(bx - sx) > 0.5f || fabsf(by - sy) > 0.5f) screen_ok = 0;
                    }
                }
                for (float wx = 1700.0f; wx < 2400.0f; wx += 97.0f) {
                    for (float wy = 800.0f; wy < 1250.0f; wy += 83.0f) {
                        float sx, sy, depth, px, py;
                        /* Only what is on screen can be clicked. Zoomed in
                         * close, most of this grid is not, and some of it is
                         * behind the eye. */
                        if (!camera_view_project(&v, wx, wy, 0.0f, &sx, &sy, &depth)) continue;
                        if (sx < 0.0f || sx > VW || sy < 0.0f || sy > VH) continue;
                        if (!camera_view_pick_ground(&v, sx, sy, &px, &py)) { world_ok = 0; continue; }
                        if (fabsf(px - wx) > 0.5f || fabsf(py - wy) > 0.5f) world_ok = 0;
                    }
                }
            }
        }
        CHECK(screen_ok, "screen -> world -> screen lands within half a pixel");
        CHECK(world_ok,  "world -> screen -> world lands within half a unit");
    }

    printf("\nTEST 3: north is up and east is right before the camera turns\n");
    {
        CameraView v;
        build_3d(&v, 1000.0f, 1000.0f, 0.0f, 1.0f);
        float sx, sy, depth;
        camera_view_project(&v, 1000.0f, 900.0f, 0.0f, &sx, &sy, &depth);
        CHECK(sy < VH / 2.0f && fabsf(sx - VW / 2.0f) < 0.5f, "a point to the north is straight up");
        camera_view_project(&v, 1100.0f, 1000.0f, 0.0f, &sx, &sy, &depth);
        CHECK(sx > VW / 2.0f && fabsf(sy - VH / 2.0f) < 0.5f, "a point to the east is straight right");
        camera_view_project(&v, 1000.0f, 1000.0f, 40.0f, &sx, &sy, &depth);
        CHECK(sy < VH / 2.0f, "height goes up the screen");
    }

    printf("\nTEST 4: the movement keys agree with the screen at every yaw\n");
    {
        int forward_ok = 1, right_ok = 1, unit_ok = 1;
        for (int i = 0; i < YAW_COUNT; i++) {
            float yaw = rad(YAWS_DEG[i]);
            CameraView v;
            build_3d(&v, 3000.0f, 3000.0f, YAWS_DEG[i], 1.0f);

            float wx, wy, sx, sy, depth;
            camera_math_rotate_input(yaw, 0.0f, -1.0f, &wx, &wy);   /* W */
            if (fabsf(wx * wx + wy * wy - 1.0f) > 0.001f) unit_ok = 0;
            camera_view_project(&v, 3000.0f + wx * 100.0f, 3000.0f + wy * 100.0f, 0.0f,
                                &sx, &sy, &depth);
            if (!(sy < VH / 2.0f - 10.0f) || fabsf(sx - VW / 2.0f) > 0.5f) forward_ok = 0;

            camera_math_rotate_input(yaw, 1.0f, 0.0f, &wx, &wy);    /* D */
            camera_view_project(&v, 3000.0f + wx * 100.0f, 3000.0f + wy * 100.0f, 0.0f,
                                &sx, &sy, &depth);
            if (!(sx > VW / 2.0f + 10.0f) || fabsf(sy - VH / 2.0f) > 0.5f) right_ok = 0;
        }
        CHECK(forward_ok, "W walks straight up the screen");
        CHECK(right_ok,   "D walks straight right on the screen");
        CHECK(unit_ok,    "a single key is a unit direction, so speed does not change");

        float wx, wy;
        camera_math_rotate_input(0.0f, 1.0f, -1.0f, &wx, &wy);
        CHECK(fabsf(wx - 1.0f) < 0.001f && fabsf(wy + 1.0f) < 0.001f,
              "at yaw 0 the input passes through unchanged");
    }

    printf("\nTEST 5: zoom keeps the old pixel scale at the player\n");
    {
        CameraView v;
        build_3d(&v, 800.0f, 800.0f, 30.0f, 1.0f);
        float ppu1 = camera_view_pixels_per_unit(&v, 800.0f, 800.0f, 0.0f);
        build_3d(&v, 800.0f, 800.0f, 30.0f, 2.0f);
        float ppu2 = camera_view_pixels_per_unit(&v, 800.0f, 800.0f, 0.0f);
        CHECK(fabsf(ppu1 - 1.0f) < 0.01f, "zoom 1 is one pixel per unit at the player");
        CHECK(fabsf(ppu2 - 2.0f) < 0.02f, "zoom 2 is two pixels per unit at the player");

        build_3d(&v, 800.0f, 800.0f, 0.0f, 1.0f);
        float far_ppu  = camera_view_pixels_per_unit(&v, 800.0f, 400.0f, 0.0f);
        float near_ppu = camera_view_pixels_per_unit(&v, 800.0f, 1000.0f, 0.0f);
        CHECK(far_ppu < 1.0f && near_ppu > 1.0f, "things further away are drawn smaller");
    }

    printf("\nTEST 6: the ground bounds cover the screen and fit what is streamed\n");
    {
        int covers = 1, fits = 1;
        /* A player at a chunk corner has LOAD_RADIUS_CHUNKS whole chunks
         * streamed on every side; that is the least the view can rely on. The
         * camera also trails the player a little, hence the margin. */
        float streamed = (float)(LOAD_RADIUS_CHUNKS * CHUNK_SIZE) * TILE - 2.0f * TILE;
        for (int i = 0; i < YAW_COUNT; i++) {
            for (int z = 0; z < 2; z++) {
                float zoom = z ? CAMERA_ZOOM_MAX : CAMERA_ZOOM_MIN;
                CameraView v;
                build_3d(&v, 9000.0f, 9000.0f, YAWS_DEG[i], zoom);
                float x0, y0, x1, y1;
                camera_view_ground_bounds(&v, &x0, &y0, &x1, &y1);
                for (float sx = 0.0f; sx <= VW; sx += VW / 8.0f) {
                    for (float sy = 0.0f; sy <= VH; sy += VH / 8.0f) {
                        float wx, wy;
                        if (!camera_view_pick_ground(&v, sx, sy, &wx, &wy)) { covers = 0; continue; }
                        if (wx < x0 - 0.5f || wx > x1 + 0.5f || wy < y0 - 0.5f || wy > y1 + 0.5f)
                            covers = 0;
                    }
                }
                if (9000.0f - x0 > streamed || x1 - 9000.0f > streamed ||
                    9000.0f - y0 > streamed || y1 - 9000.0f > streamed)
                    fits = 0;
            }
        }
        CHECK(covers, "every visible point is inside the bounds");
        CHECK(fits,   "the furthest visible ground is always streamed in");
    }

    printf("\nTEST 7: depth orders what is nearer the camera first\n");
    {
        CameraView v;
        build_3d(&v, 500.0f, 500.0f, 0.0f, 1.0f);
        float sx, sy, near_d, far_d;
        camera_view_project(&v, 500.0f, 560.0f, 0.0f, &sx, &sy, &near_d);
        camera_view_project(&v, 500.0f, 440.0f, 0.0f, &sx, &sy, &far_d);
        CHECK(near_d < far_d, "south of the player is nearer at yaw 0");
        CHECK(near_d > -1.0f && far_d < 1.0f, "both sit inside the depth range");
    }

    printf("\nTEST 8: the free camera frames the body and stays above ground\n");
    {
        int centred = 1, above = 1, sees_player = 1;
        const float pitches[] = { CAMERA_PITCH_MIN_DEG, -10.0f, 0.0f, 30.0f,
                                  CAMERA_PITCH_DEFAULT_DEG, CAMERA_PITCH_MAX_DEG };
        const int pitch_count = (int)(sizeof(pitches) / sizeof(pitches[0]));
        const float zooms[] = { CAMERA_ZOOM_MIN, CAMERA_ZOOM_DEFAULT, CAMERA_ZOOM_MAX };
        for (int p = 0; p < pitch_count; p++) {
            for (int z = 0; z < 3; z++) {
                for (int i = 0; i < YAW_COUNT; i++) {
                    float fovy = rad(CAMERA_FOVY_DEG);
                    float dist = camera_math_floor_distance(
                        rad(pitches[p]), CAMERA_TARGET_HEIGHT,
                        camera_math_distance_for_zoom(zooms[z], VH, fovy), CAMERA_MIN_EYE_HEIGHT);
                    CameraView v;
                    camera_view_build(&v, 700.0f, 900.0f, CAMERA_TARGET_HEIGHT, rad(YAWS_DEG[i]),
                                      rad(pitches[p]), dist, fovy, VW, VH);
                    float sx, sy, d;
                    if (!camera_view_project(&v, 700.0f, 900.0f, CAMERA_TARGET_HEIGHT, &sx, &sy, &d) ||
                        fabsf(sx - VW / 2.0f) > 0.5f || fabsf(sy - VH / 2.0f) > 0.5f)
                        centred = 0;
                    if (CAMERA_TARGET_HEIGHT + dist * sinf(rad(pitches[p])) <
                        CAMERA_MIN_EYE_HEIGHT - 0.01f)
                        above = 0;
                    /* The feet and the head are both on screen and in depth
                     * -- from above. Looking up from the floor, close in, the
                     * body can overfill the view, as it does in FF14. */
                    for (int h = 0; h <= 1 && pitches[p] >= 0.0f; h++) {
                        if (!camera_view_project(&v, 700.0f, 900.0f, h ? 40.0f : 0.0f, &sx, &sy, &d) ||
                            sx < 0.0f || sx > VW || sy < 0.0f || sy > VH || d <= -1.0f || d >= 1.0f)
                            sees_player = 0;
                    }
                }
            }
        }
        CHECK(centred, "the chest of the character is the centre of the screen");
        CHECK(above, "the eye never goes under the ground, at any tilt or zoom");
        CHECK(sees_player, "the whole character is in view at every pitch and zoom");
        float wide = camera_math_distance_for_zoom(CAMERA_ZOOM_MIN, VH, rad(CAMERA_FOVY_DEG));
        float close = camera_math_distance_for_zoom(CAMERA_ZOOM_MAX, VH, rad(CAMERA_FOVY_DEG));
        CHECK(close < 100.0f && wide > 1000.0f, "zoom runs from over the shoulder to far overhead");
        CHECK(CAMERA_PITCH_MIN_DEG < 0.0f, "the camera can tilt below the character, looking up");
        float slid = camera_math_floor_distance(rad(-30.0f), 24.0f, 500.0f, 4.0f);
        CHECK(fabsf(24.0f - slid * 0.5f - 4.0f) < 0.01f,
              "tilted below, a far camera slides in until it rests on the floor");
        CHECK(camera_math_floor_distance(rad(-30.0f), 24.0f, 20.0f, 4.0f) == 20.0f,
              "a close one already above the floor is left where it is");
        CHECK(camera_math_floor_distance(rad(40.0f), 24.0f, 900.0f, 4.0f) == 900.0f,
              "and looking down, distance is never shortened");
    }

    printf("\n");
    if (g_failures) {
        printf("%d ASSERTION%s FAILED\n", g_failures, g_failures == 1 ? "" : "S");
        return 1;
    }
    printf("ALL ASSERTIONS PASSED\n");
    return 0;
}
