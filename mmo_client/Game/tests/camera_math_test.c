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

typedef struct { float x_from, top; } TestWall;

/** A solid wall `top` tall for every point with x >= x_from. */
static float wall_height(void* ctx, float wx, float wy) {
    (void)wy;
    const TestWall* w = ctx;
    return wx >= w->x_from ? w->top : 0.0f;
}

/** Free walkable air: a stair tile out to x = 16, under a ceiling at h = 40. */
static int stair_air(void* ctx, float wx, float wy, float h) {
	(void)ctx;
	(void)wy;
	return wx < 16.0f && h < 40.0f;
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
                        camera_math_distance_for_zoom(zooms[z], VH, fovy), CAMERA_MIN_EYE_HEIGHT, 0.0f);
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
        float slid = camera_math_floor_distance(rad(-30.0f), 24.0f, 500.0f, 4.0f, 0.0f);
        CHECK(fabsf(24.0f - slid * 0.5f - 4.0f) < 0.01f,
              "tilted below, a far camera slides in until it rests on the floor");
        CHECK(camera_math_floor_distance(rad(-30.0f), 24.0f, 20.0f, 4.0f, 0.0f) == 20.0f,
              "a close one already above the floor is left where it is");
        CHECK(camera_math_floor_distance(rad(40.0f), 24.0f, 900.0f, 4.0f, 0.0f) == 900.0f,
              "and looking down, distance is never shortened");
        /* A floor at 100: the target stands 24 above it, and an eye that
         * would sit at 90 is pulled up to rest 4 above the floor. */
        float raised = camera_math_floor_distance(rad(-30.0f), 124.0f, 500.0f, 4.0f, 100.0f);
        CHECK(fabsf(124.0f - raised * 0.5f - 104.0f) < 0.01f,
              "over a raised floor the eye rests on that floor, not on the ground");
        CHECK(camera_math_floor_distance(rad(-30.0f), 124.0f, 500.0f, 4.0f, 100.0f) ==
              camera_math_floor_distance(rad(-30.0f), 24.0f, 500.0f, 4.0f, 0.0f),
              "the same framing at any height");
    }

    printf("\nTEST 9: the camera stops short of a wall between it and the player\n");
    {
        /* A wall 60 units tall along x >= 100; the player stands at x = 0 and the
         * camera orbits to the east (yaw 90 degrees puts the eye at +x). */
        TestWall wall = { 100.0f, 60.0f };
        const float yaw_east = rad(90.0f);
        const float low = rad(10.0f);

        float stop = camera_math_clear_distance(0.0f, 0.0f, 24.0f, yaw_east, low, 600.0f,
		                                        4.0f, 10.0f, wall_height, NULL, &wall);
        float hit = wall.x_from / cosf(low);   /* eye distance at which it reaches the wall */
        CHECK(stop < hit - 10.0f && stop >= hit - 14.0f - 0.01f,
              "the eye halts a margin before the wall");

        CHECK(camera_math_clear_distance(0.0f, 0.0f, 24.0f, rad(270.0f), low, 600.0f,
		                                 4.0f, 10.0f, wall_height, NULL, &wall) == 600.0f,
              "looking away from the wall, nothing is in the way");

        wall.x_from = 20.0f;
        CHECK(camera_math_clear_distance(0.0f, 0.0f, 24.0f, yaw_east, rad(85.0f), 600.0f,
		                                 4.0f, 10.0f, wall_height, NULL, &wall) == 600.0f,
              "looking steeply down, the eye rises over a wall rather than sitting behind it");
        CHECK(camera_math_clear_distance(0.0f, 0.0f, 24.0f, yaw_east, low, 600.0f,
		                                 4.0f, 10.0f, NULL, NULL, NULL) == 600.0f,
              "without a height source the distance is untouched");

        wall.x_from = 3.0f;
        CHECK(camera_math_clear_distance(0.0f, 0.0f, 24.0f, yaw_east, low, 600.0f,
		                                 4.0f, 10.0f, wall_height, NULL, &wall) == 0.0f,
              "a wall against the player shortens it to nothing, never below");

        wall.x_from = 100.0f;
        wall.top = 10.0f;   /* a fence under the player's chest (24 units) */
        CHECK(camera_math_clear_distance(0.0f, 0.0f, 24.0f, yaw_east, low, 600.0f,
		                                 4.0f, 10.0f, wall_height, NULL, &wall) == 600.0f,
              "a low fence does not pull the camera in");

		// A stair tile beside a tall wall: the air above its floor is not wall
		wall.x_from = 3.0f;
		wall.top = 300.0f;
		float into = camera_math_clear_distance(0.0f, 0.0f, 24.0f, yaw_east, low, 600.0f,
		                                        4.0f, 10.0f, wall_height, stair_air, &wall);
		float reach = 16.0f / cosf(low);       // eye distance at the stair tile's edge
		CHECK(into > 3.0f / cosf(low) && into < reach - 10.0f && into >= reach - 14.0f - 0.01f,
		      "free walkable air is not a wall: the eye stops at the real wall past the tile");
		CHECK(camera_math_clear_distance(0.0f, 0.0f, 24.0f, yaw_east, rad(60.0f), 600.0f,
		                                 4.0f, 10.0f, wall_height, stair_air, &wall) < 600.0f,
		      "above the ceiling of that air the solid counts again");
	}

    printf("\nTEST 10: the screen ray runs from the eye through the picked point\n");
    {
        int through = 1, unit = 1, agrees = 1;
        const float fovy = rad(CAMERA_FOVY_DEG);
        for (int i = 0; i < YAW_COUNT; i++) {
            CameraView v;
            camera_view_build(&v, 700.0f, 900.0f, CAMERA_TARGET_HEIGHT, rad(YAWS_DEG[i]),
                              rad(CAMERA_PITCH_DEFAULT_DEG), 400.0f, fovy, VW, VH);
            float o[3], d[3];
            camera_view_screen_ray(&v, VW / 2.0f, VH / 2.0f, o, d);

            /* world coordinates (x, y, z up): the look-at point is (700, 900, 24) */
            float to[3] = { 700.0f - o[0], 900.0f - o[1], CAMERA_TARGET_HEIGHT - o[2] };
            float along = to[0] * d[0] + to[1] * d[1] + to[2] * d[2];
            float miss[3] = { to[0] - along * d[0], to[1] - along * d[1], to[2] - along * d[2] };
            if (sqrtf(miss[0] * miss[0] + miss[1] * miss[1] + miss[2] * miss[2]) > 0.5f) through = 0;
            if (fabsf(sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]) - 1.0f) > 1e-3f) unit = 0;

            /* the ground the old pick found is where the ray crosses z = 0 */
            float gx, gy;
            if (camera_view_pick_ground(&v, 300.0f, 200.0f, &gx, &gy)) {
                float ro[3], rd[3];
                camera_view_screen_ray(&v, 300.0f, 200.0f, ro, rd);
                float t = -ro[2] / rd[2];
                if (fabsf(ro[0] + t * rd[0] - gx) > 0.5f || fabsf(ro[1] + t * rd[1] - gy) > 0.5f)
                    agrees = 0;
            }
        }
        CHECK(through, "the centre ray passes through the look-at point at every yaw");
        CHECK(unit, "the ray direction is a unit vector");
        CHECK(agrees, "the ray meets the ground where camera_view_pick_ground does");
    }

	printf("\nTEST 11: the eye sits the orbit distance from the target, above it\n");
	{
		int near = 1, above = 1;

		for(int i = 0; i < YAW_COUNT; i++) {
			CameraView v;
			float eye[3];
			float dx;
			float dy;
			float dz;

			camera_view_build(&v, 700.0f, 900.0f, CAMERA_TARGET_HEIGHT, rad(YAWS_DEG[i]),
			                  rad(CAMERA_PITCH_DEFAULT_DEG), 400.0f, rad(CAMERA_FOVY_DEG), VW, VH);
			camera_view_eye(&v, eye);
			// GL space: (x, up, z) where the world point (700, 900) is x = 700, z = 900
			dx = eye[0] - 700.0f;
			dy = eye[1] - CAMERA_TARGET_HEIGHT;
			dz = eye[2] - 900.0f;
			if(fabsf(sqrtf(dx * dx + dy * dy + dz * dz) - 400.0f) > 2.0f)
				near = 0;
			if(dy < 400.0f * sinf(rad(CAMERA_PITCH_DEFAULT_DEG)) - 2.0f)
				above = 0;
		}
		CHECK(near, "the eye is 400 units from the target at every yaw");
		CHECK(above, "the eye is as far above the target as the pitch says");
	}

    printf("\n");
    if (g_failures) {
        printf("%d ASSERTION%s FAILED\n", g_failures, g_failures == 1 ? "" : "S");
        return 1;
    }
    printf("ALL ASSERTIONS PASSED\n");
    return 0;
}
