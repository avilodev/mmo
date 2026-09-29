/**
 * @file
 * Check how a body chooses, times and blends its clip from its movement.
 *
 * Every mistake here is a character that moonwalks, spins, or stutters between
 * standing and walking every time a 20 Hz position update lands -- all things
 * that only show up with several clients connected. So the cases are the
 * inputs the game actually produces: the local player's smooth per-frame
 * motion, a remote body's stepped 20 Hz motion, a respawn, and a quest NPC
 * standing still while somebody talks to it.
 */

#include "render/character_motion.h"
#include "render/character_tuning.h"

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

#define DT (1.0f / 60.0f)
#define PI_F 3.14159265f

static const float DURATIONS[CHARACTER_ANIM_COUNT] = { 2.0f, 1.0f, 0.6f, 3.0f, 0.9f };

/** Move a body at a constant velocity for `seconds`, one update per frame. */
static void run_for(CharacterMotion* m, float* x, float* y, float vx, float vy,
                    float seconds, const CharacterHints* hints) {
    int frames = (int)(seconds / DT + 0.5f);
    for (int i = 0; i < frames; i++) {
        *x += vx * DT;
        *y += vy * DT;
        character_motion_update(m, *x, *y, DT, hints, DURATIONS);
    }
}

static float angle_between(float a, float b) {
    float d = fmodf(fabsf(a - b), 2.0f * PI_F);
    return d > PI_F ? 2.0f * PI_F - d : d;
}

int main(void) {
    printf("=== character motion ===\n");
    CharacterMotion m;
    float x, y;

    printf("\nTEST 1: standing still is idle\n");
    x = 100.0f; y = 100.0f;
    character_motion_reset(&m, x, y, 0.0f);
    run_for(&m, &x, &y, 0.0f, 0.0f, 1.0f, NULL);
    CHECK(m.anim == CHARACTER_ANIM_IDLE, "idle after a second of standing");
    CHECK(m.speed < 0.01f, "speed reads zero");

    printf("\nTEST 2: the player's run speed runs, a stroll walks\n");
    run_for(&m, &x, &y, 200.0f, 0.0f, 0.5f, NULL);
    CHECK(m.anim == CHARACTER_ANIM_RUN, "200 units/s is a run");
    CHECK(fabsf(m.speed - 200.0f) < 5.0f, "measured speed settles on the real one");
    run_for(&m, &x, &y, 30.0f, 0.0f, 1.0f, NULL);
    CHECK(m.anim == CHARACTER_ANIM_WALK, "30 units/s is a walk");
    run_for(&m, &x, &y, 0.0f, 0.0f, 1.0f, NULL);
    CHECK(m.anim == CHARACTER_ANIM_IDLE, "stopping returns to idle");

    printf("\nTEST 3: the body turns to face the way it goes (D23)\n");
    character_motion_reset(&m, x, y, 0.0f);          /* facing +y, south */
    run_for(&m, &x, &y, 0.0f, -150.0f, 0.5f, NULL);  /* north */
    CHECK(angle_between(m.yaw, PI_F) < 0.01f, "running north faces north");
    run_for(&m, &x, &y, 150.0f, 0.0f, 0.5f, NULL);   /* east */
    CHECK(angle_between(m.yaw, PI_F / 2.0f) < 0.01f, "running east faces east");
    {
        CharacterMotion turn;
        float tx = 0.0f, ty = 0.0f;
        character_motion_reset(&turn, tx, ty, 0.0f);
        run_for(&turn, &tx, &ty, 0.0f, -150.0f, DT, NULL);
        float per_frame = CHARACTER_TURN_DEG_PER_SEC * PI_F / 180.0f * DT;
        CHECK(angle_between(turn.yaw, 0.0f) <= per_frame + 1e-4f,
              "a reversal turns at the turn rate, not in one frame");
    }

    printf("\nTEST 4: a remote body's 20 Hz steps read as steady movement\n");
    {
        CharacterMotion r;
        float rx = 0.0f, ry = 0.0f;
        character_motion_reset(&r, rx, ry, 0.0f);
        int flickers = 0, moving_frames = 0;
        for (int frame = 0; frame < 120; frame++) {
            if (frame % 3 == 0) rx += 150.0f * DT * 3.0f;   /* one update every third frame */
            character_motion_update(&r, rx, ry, DT, NULL, DURATIONS);
            if (frame >= 30) {
                moving_frames++;
                if (r.anim == CHARACTER_ANIM_IDLE) flickers++;
            }
        }
        CHECK(moving_frames > 0 && flickers == 0, "never drops to idle between updates");
    }

    printf("\nTEST 5: a respawn does not register as a sprint\n");
    {
        CharacterMotion t;
        character_motion_reset(&t, 0.0f, 0.0f, 0.0f);
        character_motion_update(&t, 5000.0f, 5000.0f, DT, NULL, DURATIONS);
        CHECK(t.anim == CHARACTER_ANIM_IDLE && t.speed < 1.0f, "a teleport stays idle");
    }

    printf("\nTEST 6: a standing NPC talks and turns to whoever is near\n");
    {
        CharacterMotion n;
        character_motion_reset(&n, 0.0f, 0.0f, 0.0f);
        CharacterHints hints = { 0 };
        hints.talking  = 1;
        hints.has_look = 1;
        hints.look_x   = -50.0f;   /* due west */
        hints.look_y   = 0.0f;
        float nx = 0.0f, ny = 0.0f;
        run_for(&n, &nx, &ny, 0.0f, 0.0f, 2.0f, &hints);
        CHECK(n.anim == CHARACTER_ANIM_TALK, "talking plays the talk clip");
        CHECK(angle_between(n.yaw, -PI_F / 2.0f) < 0.01f, "and faces the player");
    }

    printf("\nTEST 7: a clip change cross-fades, and times wrap\n");
    {
        CharacterMotion b;
        float bx = 0.0f, by = 0.0f;
        character_motion_reset(&b, bx, by, 0.0f);
        run_for(&b, &bx, &by, 200.0f, 0.0f, DT * 12.0f, NULL);
        CHECK(b.prev != b.anim, "the previous clip is remembered");
        run_for(&b, &bx, &by, 200.0f, 0.0f, CHARACTER_BLEND_TIME + 0.1f, NULL);
        CHECK(b.blend >= 1.0f, "the fade completes after the blend time");
        run_for(&b, &bx, &by, 200.0f, 0.0f, 5.0f, NULL);
        CHECK(b.anim_time >= 0.0f && b.anim_time < DURATIONS[b.anim], "clip time stays in range");
    }

    printf("\nTEST 8: playback rate follows speed within limits\n");
    CHECK(fabsf(character_motion_clip_rate(CHARACTER_ANIM_RUN, CHARACTER_RUN_CLIP_SPEED) - 1.0f) < 1e-5f,
          "the clip's own speed plays at 1x");
    CHECK(character_motion_clip_rate(CHARACTER_ANIM_WALK, 1.0f) == CHARACTER_CLIP_RATE_MIN,
          "a crawl clamps to the minimum rate");
    CHECK(character_motion_clip_rate(CHARACTER_ANIM_RUN, 10000.0f) == CHARACTER_CLIP_RATE_MAX,
          "a blur clamps to the maximum rate");
    CHECK(character_motion_clip_rate(CHARACTER_ANIM_IDLE, 500.0f) == 1.0f, "idle is never rescaled");

    printf("\nTEST 9: the local body takes its facing and pivot from locomotion\n");
    {
        CharacterMotion p;
        character_motion_reset(&p, 0.0f, 0.0f, 0.0f);
        CharacterHints hints = { 0 };
        hints.has_facing = 1;
        hints.facing_yaw = 2.0f;
        hints.pivoting   = 1;
        character_motion_update(&p, 0.0f, 0.0f, DT, &hints, DURATIONS);
        CHECK(fabsf(p.yaw - 2.0f) < 1e-5f, "a known facing is used as is, not turned toward");
        CHECK(p.anim == CHARACTER_ANIM_WALK, "turning on the spot steps rather than stands");
        hints.pivoting = 0;
        float px = 0.0f, py = 0.0f;
        run_for(&p, &px, &py, 0.0f, 0.0f, 0.5f, &hints);
        CHECK(p.anim == CHARACTER_ANIM_IDLE, "and stands once the pivot ends");
        run_for(&p, &px, &py, 0.0f, -64.0f, 1.0f, &hints);
        CHECK(p.anim == CHARACTER_ANIM_WALK, "the locomotion walk speed plays the walk");
        run_for(&p, &px, &py, 0.0f, -150.0f, 1.0f, &hints);
        CHECK(p.anim == CHARACTER_ANIM_JOG, "run mode's speed plays the jog");
        run_for(&p, &px, &py, 0.0f, -200.0f, 1.0f, &hints);
        CHECK(p.anim == CHARACTER_ANIM_RUN, "and sprint's the sprint");
    }

    printf("\n%s: %d failure(s)\n", g_failures ? "FAILED" : "PASSED", g_failures);
    return g_failures ? 1 : 0;
}
