/**
 * @file
 * Check the local player's GTA-style locomotion, without a window.
 *
 * What is pinned is what a player feels: walk, run and sprint each have their
 * own pace; speed
 * builds and falls away instead of snapping; the body goes where it faces;
 * reversing from a standstill turns on the spot instead of backpedalling;
 * reversing at a run brakes, turns and sets off again. And, because the
 * server validates every step, the body is never faster than max_speed.
 */

#include "player/locomotion.h"

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

#define DT      (1.0f / 60.0f)
#define MAX     200.0f
#define PI_F    3.14159265f

/* World directions: north is -y. heading 0 faces south (+y). */
#define NORTH_X 0.0f
#define NORTH_Y -1.0f
#define SOUTH_X 0.0f
#define SOUTH_Y 1.0f

typedef struct { float x, y, max_speed_seen, max_back; } Track;

/** Run `seconds` of frames holding one direction, tracking the path. */
static void hold(Locomotion* l, Track* t, float ix, float iy, LocoMode mode, float seconds) {
    int frames = (int)(seconds / DT + 0.5f);
    for (int i = 0; i < frames; i++) {
        float dx, dy;
        locomotion_step(l, ix, iy, mode, MAX, DT, &dx, &dy);
        t->x += dx;
        t->y += dy;
        float v = sqrtf(dx * dx + dy * dy) / DT;
        if (v > t->max_speed_seen) t->max_speed_seen = v;
    }
}

static float angle_between(float a, float b) {
    float d = fmodf(fabsf(a - b), 2.0f * PI_F);
    return d > PI_F ? 2.0f * PI_F - d : d;
}

int main(void) {
    printf("=== locomotion ===\n");

    printf("\nTEST 1: each mode has its own pace, sprint the fastest\n");
    {
        Locomotion l = { PI_F, 0.0f, 0 };   /* facing north */
        Track t = { 0 };
        hold(&l, &t, NORTH_X, NORTH_Y, LOCO_MODE_RUN, 1.0f);
        CHECK(fabsf(l.speed - MAX * LOCO_RUN_FRACTION) < 0.5f, "run settles at running speed");
        hold(&l, &t, NORTH_X, NORTH_Y, LOCO_MODE_WALK, 1.0f);
        CHECK(fabsf(l.speed - MAX * LOCO_WALK_FRACTION) < 0.5f, "walk slows to walking speed");
        hold(&l, &t, NORTH_X, NORTH_Y, LOCO_MODE_SPRINT, 1.0f);
        CHECK(fabsf(l.speed - MAX) < 0.5f, "sprint reaches the top speed");
        CHECK(locomotion_mode_speed(LOCO_MODE_WALK, MAX) < locomotion_mode_speed(LOCO_MODE_RUN, MAX) &&
              locomotion_mode_speed(LOCO_MODE_RUN, MAX) < locomotion_mode_speed(LOCO_MODE_SPRINT, MAX),
              "walk < run < sprint");
        CHECK(locomotion_mode_speed(LOCO_MODE_SPRINT, MAX) <= MAX, "and sprint is within the server's limit");
        CHECK(t.y < 0.0f && fabsf(t.x) < 0.01f, "and all of it went north");
    }

    printf("\nTEST 2: speed builds and falls away\n");
    {
        Locomotion l = { PI_F, 0.0f, 0 };
        Track t = { 0 };
        hold(&l, &t, NORTH_X, NORTH_Y, LOCO_MODE_SPRINT, DT);
        CHECK(l.speed > 0.0f && l.speed < MAX * 0.2f, "one frame in, not yet at full speed");
        hold(&l, &t, NORTH_X, NORTH_Y, LOCO_MODE_SPRINT, 0.5f);
        CHECK(fabsf(l.speed - MAX) < 0.5f, "full speed within half a second");
        hold(&l, &t, 0.0f, 0.0f, LOCO_MODE_WALK, DT);
        CHECK(l.speed > 0.0f, "releasing the keys does not stop dead");
        hold(&l, &t, 0.0f, 0.0f, LOCO_MODE_WALK, 0.3f);
        CHECK(l.speed == 0.0f, "but stops within a fraction of a second");
    }

    printf("\nTEST 3: reversing from a standstill turns on the spot\n");
    {
        Locomotion l = { PI_F, 0.0f, 0 };   /* standing, facing north */
        Track t = { 0 };
        int frames_to_face = 0;
        float moved_before_facing = 0.0f;
        for (int i = 0; i < 60; i++) {
            float dx, dy;
            locomotion_step(&l, SOUTH_X, SOUTH_Y, LOCO_MODE_WALK, MAX, DT, &dx, &dy);
            t.x += dx; t.y += dy;
            if (angle_between(l.heading, 0.0f) > 0.35f) {
                frames_to_face++;
                moved_before_facing += sqrtf(dx * dx + dy * dy);
                if (dy < 0.0f) t.max_back += -dy;   /* stepping north = backwards */
            }
        }
        CHECK(frames_to_face > 1, "the turn takes a few frames, not one");
        CHECK(frames_to_face < 20, "but is quick: well under a third of a second");
        CHECK(moved_before_facing < 1.0f, "and the body does not move while turning");
        CHECK(t.max_back < 0.01f, "it never steps backwards");
        CHECK(t.y > 0.0f && angle_between(l.heading, 0.0f) < 0.05f, "then walks off south");
    }

    printf("\nTEST 4: reversing at a run brakes, turns, and sets off again\n");
    {
        Locomotion l = { PI_F, 0.0f, 0 };
        Track t = { 0 };
        hold(&l, &t, NORTH_X, NORTH_Y, LOCO_MODE_SPRINT, 1.0f);    /* running north */
        float y_before = t.y;
        float dx, dy;
        locomotion_step(&l, SOUTH_X, SOUTH_Y, LOCO_MODE_SPRINT, MAX, DT, &dx, &dy);
        CHECK(dy < 0.0f, "the first frame still carries north (momentum)");
        CHECK(l.speed < MAX, "and is already braking");
        Track u = { 0 };
        hold(&l, &u, SOUTH_X, SOUTH_Y, LOCO_MODE_SPRINT, 0.4f);
        CHECK(fabsf(u.y) < 30.0f, "it skids to a stop and turns instead of sprinting backwards");
        hold(&l, &u, SOUTH_X, SOUTH_Y, LOCO_MODE_SPRINT, 1.0f);
        CHECK(fabsf(l.speed - MAX) < 0.5f && angle_between(l.heading, 0.0f) < 0.05f,
              "then it is running south");
        CHECK(t.y + u.y > y_before, "and has come back past where it turned");
    }

    printf("\nTEST 5: a running turn is a curve\n");
    {
        Locomotion l = { PI_F, 0.0f, 0 };
        Track t = { 0 };
        hold(&l, &t, NORTH_X, NORTH_Y, LOCO_MODE_SPRINT, 1.0f);
        float x0 = t.x, y0 = t.y;
        hold(&l, &t, 1.0f, 0.0f, LOCO_MODE_SPRINT, DT);             /* now east */
        CHECK(angle_between(l.heading, PI_F / 2.0f) > 0.5f, "one frame does not complete a 90 degree turn");
        hold(&l, &t, 1.0f, 0.0f, LOCO_MODE_SPRINT, 1.0f);
        CHECK(angle_between(l.heading, PI_F / 2.0f) < 0.01f, "it comes round to face east");
        CHECK(t.y < y0 - 1.0f && t.x > x0, "carrying on north a little while it turned east");
    }

    printf("\nTEST 6: never faster than the server allows\n");
    {
        Locomotion l = { 0.0f, 0.0f, 0 };
        Track t = { 0 };
        const float dirs[5][2] = { { 0, -1 }, { 1, 1 }, { 0, 1 }, { -1, 0 }, { 0.3f, -0.9f } };
        for (int k = 0; k < 40; k++)
            hold(&l, &t, dirs[k % 5][0], dirs[k % 5][1], (LocoMode)(k % LOCO_MODE_COUNT), 0.15f);
        CHECK(t.max_speed_seen <= MAX + 0.01f, "no frame exceeds max_speed");
        Locomotion h = { 1.0f, 150.0f, 1 };
        locomotion_halt(&h);
        CHECK(h.speed == 0.0f && h.pivoting == 0 && h.heading == 1.0f, "halt stops but keeps the heading");
    }

    printf("\n%s: %d failure(s)\n", g_failures ? "FAILED" : "PASSED", g_failures);
    return g_failures ? 1 : 0;
}
