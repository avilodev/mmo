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
#include <string.h>

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

static const float DURATIONS[CHARACTER_ANIM_COUNT] = {
	[CHARACTER_ANIM_IDLE] = 2.0f, [CHARACTER_ANIM_WALK] = 1.0f,
	[CHARACTER_ANIM_RUN] = 0.6f, [CHARACTER_ANIM_TALK] = 3.0f,
	[CHARACTER_ANIM_JOG] = 0.9f, [CHARACTER_ANIM_SIT_ENTER] = 1.0f,
	[CHARACTER_ANIM_SIT_IDLE] = 2.0f, [CHARACTER_ANIM_SIT_EXIT] = 0.5f,
	[CHARACTER_ANIM_DANCE] = 3.0f, [CHARACTER_ANIM_FIDGET] = 2.5f,
	[CHARACTER_ANIM_PIVOT_LEFT] = 0.5f, [CHARACTER_ANIM_PIVOT_RIGHT] = 0.5f,
};

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

// Move a body along its facing at `speed` while it turns at `rate` rad/s
static void run_turning(CharacterMotion* m, float* x, float* y, float* heading,
                        float speed, float rate, float seconds) {
	CharacterHints hints = { 0 };
	int frames = (int)(seconds / DT + 0.5f);

	hints.has_facing = 1;
	for(int i = 0; i < frames; i++) {
		*heading += rate * DT;
		hints.facing_yaw = *heading;
		*x += sinf(*heading) * speed * DT;
		*y += cosf(*heading) * speed * DT;
		character_motion_update(m, *x, *y, DT, &hints, DURATIONS);
	}
}

// Stand a body still frame by frame until it first plays `anim`, or `limit`
// seconds pass. Returns the seconds it took, or -1 if it never did.
static float seconds_until(CharacterMotion* m, float* x, float* y, CharacterAnim anim,
                           float limit, const CharacterHints* hints) {
	int frames = (int)(limit / DT);

	for(int i = 1; i <= frames; i++) {
		character_motion_update(m, *x, *y, DT, hints, DURATIONS);
		if(m->anim == anim)
			return i * DT;
	}

	return -1.0f;
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

    printf("\nTEST 3b: a new body starts in the clip it wants, not blended from idle\n");
    {
        CharacterMotion s;
        CharacterHints h = { 0 };

        h.has_pose = 1;
        h.pose = CHARACTER_ANIM_SIT_IDLE;
        character_motion_reset(&s, 0.0f, 0.0f, 0.0f);
        character_motion_update(&s, 0.0f, 0.0f, DT, &h, DURATIONS);
        CHECK(s.anim == CHARACTER_ANIM_SIT_IDLE, "a seated body is seated on its first frame");
        CHECK(s.blend >= 1.0f, "with nothing fading out");
        h.pose = CHARACTER_ANIM_DANCE;
        character_motion_update(&s, 0.0f, 0.0f, DT, &h, DURATIONS);
        CHECK(s.anim == CHARACTER_ANIM_DANCE && s.blend < 1.0f,
              "a change after that still blends");
        character_motion_reset(&s, 0.0f, 0.0f, 0.0f);
        character_motion_update(&s, 0.0f, 0.0f, DT, &h, DURATIONS);
        CHECK(s.anim == CHARACTER_ANIM_DANCE && s.blend >= 1.0f,
              "a reset body starts over the same way");
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
        CHECK(p.anim == CHARACTER_ANIM_PIVOT_LEFT, "turning on the spot plays the pivot");
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

	printf("\nTEST 10: a forced pose plays, whatever the body's speed\n");
	{
		CharacterMotion p;
		CharacterHints hints = { 0 };
		float px = 0.0f;
		float py = 0.0f;

		character_motion_reset(&p, 0.0f, 0.0f, 0.0f);
		hints.has_pose = 1;
		hints.pose = CHARACTER_ANIM_SIT_IDLE;
		run_for(&p, &px, &py, 0.0f, 0.0f, 0.5f, &hints);
		CHECK(p.anim == CHARACTER_ANIM_SIT_IDLE, "a posed body plays the pose");
		run_for(&p, &px, &py, 0.0f, -300.0f, 0.5f, &hints);
		CHECK(p.anim == CHARACTER_ANIM_SIT_IDLE, "even sliding at a sprint's pace");
		CHECK(p.speed == 0.0f, "a posed body is not walking, so its speed stays zero");
		hints.has_pose = 0;
		character_motion_update(&p, px, py, DT, &hints, DURATIONS);
		CHECK(p.anim == CHARACTER_ANIM_IDLE, "and stands the moment the pose ends");
	}

	printf("\nTEST 11: sitting down and getting up play once; sitting loops\n");
	{
		CharacterMotion p;
		CharacterHints hints = { 0 };
		float px = 0.0f;
		float py = 0.0f;

		hints.has_pose = 1;

		character_motion_reset(&p, px, py, 0.0f);
		hints.pose = CHARACTER_ANIM_SIT_ENTER;
		run_for(&p, &px, &py, 0.0f, 0.0f, 2.0f, &hints);
		CHECK(fabsf(p.anim_time - DURATIONS[CHARACTER_ANIM_SIT_ENTER]) < 1e-4f,
		      "the sit-down clip holds its last frame, not restarting");

		character_motion_reset(&p, px, py, 0.0f);
		hints.pose = CHARACTER_ANIM_SIT_EXIT;
		run_for(&p, &px, &py, 0.0f, 0.0f, 2.0f, &hints);
		CHECK(fabsf(p.anim_time - DURATIONS[CHARACTER_ANIM_SIT_EXIT]) < 1e-4f,
		      "so does the get-up clip");

		character_motion_reset(&p, px, py, 0.0f);
		hints.pose = CHARACTER_ANIM_SIT_IDLE;
		run_for(&p, &px, &py, 0.0f, 0.0f, 3.0f, &hints);
		CHECK(p.anim_time < DURATIONS[CHARACTER_ANIM_SIT_IDLE] - 0.5f,
		      "the seated idle wraps and keeps going");
	}

	printf("\nTEST 12: standing still has no lean, twist or head turn\n");
	{
		CharacterMotion b;
		float bx = 50.0f, by = 50.0f;

		character_motion_reset(&b, bx, by, 0.0f);
		run_for(&b, &bx, &by, 0.0f, 0.0f, 1.0f, NULL);
		CHECK(fabsf(b.proc.now.lean_forward) < 1e-3f, "no lean forward");
		CHECK(fabsf(b.proc.now.lean_left) < 1e-3f, "no lean sideways");
		CHECK(fabsf(b.proc.now.spine_twist) < 1e-3f, "no twist");
		CHECK(fabsf(b.proc.now.head_yaw) < 1e-3f, "no head turn");
	}

	printf("\nTEST 13: running straight leans forward, and only forward\n");
	{
		CharacterMotion b;
		float bx = 0.0f, by = 0.0f, heading = 0.0f;
		float want = 150.0f / CHARACTER_RUN_CLIP_SPEED * CHARACTER_LEAN_FWD_AT_RUN_DEG *
		             PI_F / 180.0f;

		character_motion_reset(&b, bx, by, 0.0f);
		run_turning(&b, &bx, &by, &heading, 150.0f, 0.0f, 2.0f);
		CHECK(fabsf(b.proc.now.lean_forward - want) < 0.02f,
		      "the lean settles on the speed's share of the run lean");
		CHECK(fabsf(b.proc.now.lean_left) < 1e-3f, "no sideways lean");
		CHECK(fabsf(b.proc.now.spine_twist) < 1e-3f, "no twist");
		CHECK(fabsf(b.proc.now.head_yaw) < 1e-3f, "no head turn");
	}

	printf("\nTEST 14: a running turn leans into it and the upper body leads\n");
	{
		CharacterMotion b;
		float bx = 0.0f, by = 0.0f, heading = 0.0f;
		float turn = 360.0f * PI_F / 180.0f;

		// toward increasing yaw is a turn to the character's left
		character_motion_reset(&b, bx, by, 0.0f);
		run_turning(&b, &bx, &by, &heading, 150.0f, 0.0f, 1.0f);
		run_turning(&b, &bx, &by, &heading, 150.0f, turn, 0.3f);
		CHECK(b.proc.now.lean_left > 0.07f &&
		      b.proc.now.lean_left <= CHARACTER_LEAN_SIDE_MAX_DEG * PI_F / 180.0f + 1e-4f,
		      "a left turn leans left, within the limit");
		CHECK(b.proc.now.spine_twist > 0.3f && b.proc.now.spine_twist < 0.5f,
		      "the spine leads the turn");
		CHECK(b.proc.now.head_yaw > 0.1f, "the head leads further");
		CHECK(b.proc.now.spine_twist + b.proc.now.head_yaw > b.proc.now.spine_twist,
		      "head and spine turn the same way");

		character_motion_reset(&b, bx, by, heading);
		run_turning(&b, &bx, &by, &heading, 150.0f, 0.0f, 1.0f);
		run_turning(&b, &bx, &by, &heading, 150.0f, -turn, 0.3f);
		CHECK(b.proc.now.lean_left < -0.07f, "a right turn leans right");
		CHECK(b.proc.now.spine_twist < -0.3f, "and the spine leads to the right");
		CHECK(b.proc.now.head_yaw < -0.1f, "and so does the head");

		run_turning(&b, &bx, &by, &heading, 150.0f, 0.0f, 1.0f);
		CHECK(fabsf(b.proc.now.lean_left) < 0.01f, "the lean eases off after the turn");
		CHECK(fabsf(b.proc.now.spine_twist) < 0.01f, "and so does the twist");
		CHECK(fabsf(b.proc.now.head_yaw) < 0.01f, "and the head");
	}

	printf("\nTEST 15: braking from a run tips the body back, then it recovers\n");
	{
		CharacterMotion b;
		float bx = 0.0f, by = 0.0f, heading = 0.0f, lowest = 1.0f;

		character_motion_reset(&b, bx, by, 0.0f);
		run_turning(&b, &bx, &by, &heading, 150.0f, 0.0f, 1.0f);

		for(int i = 0; i < 60; i++) {
			run_turning(&b, &bx, &by, &heading, 0.0f, 0.0f, DT);
			if(b.proc.now.lean_forward < lowest)
				lowest = b.proc.now.lean_forward;
		}

		CHECK(lowest < -0.01f, "the lean goes back past upright");
		CHECK(lowest >= -CHARACTER_LEAN_BACK_MAX_DEG * PI_F / 180.0f - 1e-4f,
		      "but not past its limit");
		CHECK(fabsf(b.proc.now.lean_forward) < 0.01f, "and it is upright again");
	}

	printf("\nTEST 16: a held body, or a teleport, shows no offsets\n");
	{
		CharacterMotion b;
		CharacterHints seat = { 0 };
		float bx = 0.0f, by = 0.0f, heading = 0.0f;

		character_motion_reset(&b, bx, by, 0.0f);
		run_turning(&b, &bx, &by, &heading, 150.0f, 6.0f, 0.5f);
		seat.has_pose = 1;
		seat.pose = CHARACTER_ANIM_SIT_IDLE;
		run_for(&b, &bx, &by, 0.0f, 0.0f, 1.0f, &seat);
		CHECK(fabsf(b.proc.now.lean_forward) < 0.01f && fabsf(b.proc.now.lean_left) < 0.01f &&
		      fabsf(b.proc.now.spine_twist) < 0.01f, "sitting settles to upright");

		character_motion_reset(&b, bx, by, 0.0f);
		heading = 0.0f;
		bx = by = 0.0f;
		run_turning(&b, &bx, &by, &heading, 150.0f, 0.0f, 1.0f);
		bx += 1000.0f;
		character_motion_update(&b, bx, by, DT, NULL, DURATIONS);
		CHECK(fabsf(b.proc.now.lean_forward) < 0.12f,
		      "a teleport does not tip the body back");
	}

	printf("\nTEST 17: an aim point twists the upper body, the head takes the rest\n");
	{
		CharacterMotion b;
		CharacterHints aim = { 0 };
		float bx = 0.0f, by = 0.0f;

		// facing +y (yaw 0); a point on +x is a quarter turn toward increasing yaw
		aim.has_aim = 1;
		aim.aim_x = 100.0f;
		aim.aim_y = 0.0f;
		character_motion_reset(&b, bx, by, 0.0f);
		run_for(&b, &bx, &by, 0.0f, 0.0f, 1.0f, &aim);
		CHECK(fabsf(b.proc.now.spine_twist - CHARACTER_AIM_TWIST_MAX_DEG * PI_F / 180.0f) < 0.02f,
		      "the spine turns as far as it may");
		CHECK(fabsf(b.proc.now.head_yaw - (0.5f * PI_F - b.proc.now.spine_twist)) < 0.02f,
		      "the head makes up the rest");
		CHECK(fabsf(b.yaw) < 1e-3f, "the legs, and the facing, do not move");

		aim.aim_x = -100.0f;
		aim.aim_y = -100.0f;
		run_for(&b, &bx, &by, 0.0f, 0.0f, 1.0f, &aim);
		CHECK(b.proc.now.head_yaw <= CHARACTER_HEAD_MAX_DEG * PI_F / 180.0f + 1e-3f,
		      "the head stops at its limit");
	}

	printf("\nTEST 18: a standing body looks at its target; a moving one does not\n");
	{
		CharacterMotion b;
		CharacterHints look = { 0 };
		float bx = 0.0f, by = 0.0f;

		look.has_look = 1;
		look.look_x = 100.0f;
		look.look_y = 0.0f;
		character_motion_reset(&b, bx, by, 0.0f);
		run_for(&b, &bx, &by, 0.0f, 0.0f, 0.1f, &look);
		CHECK(b.proc.now.head_yaw > 0.1f && b.proc.now.spine_twist > 0.0f,
		      "the head and shoulders turn first");
		run_for(&b, &bx, &by, 0.0f, 0.0f, 2.0f, &look);
		CHECK(fabsf(b.proc.now.head_yaw) < 0.01f && fabsf(b.proc.now.spine_twist) < 0.01f,
		      "then the body has turned to it and they ease off");

		character_motion_reset(&b, bx, by, 0.0f);
		look.look_x = 100.0f;
		look.look_y = 0.0f;
		for(int i = 0; i < 90; i++) {
			by += 150.0f * DT;
			character_motion_update(&b, bx, by, DT, &look, DURATIONS);
		}
		CHECK(fabsf(b.proc.now.head_yaw) < 0.01f, "running past something, the head stays ahead");
	}

	printf("\nTEST 19: stepped 20 Hz positions do not make the lean flutter\n");
	{
		CharacterMotion b;
		float bx = 0.0f, by = 0.0f, lo = 10.0f, hi = -10.0f;

		character_motion_reset(&b, bx, by, 0.0f);

		for(int i = 0; i < 180; i++) {
			if(i % 3 == 0)
				by += 150.0f * DT * 3.0f;
			character_motion_update(&b, bx, by, DT, NULL, DURATIONS);
			if(i >= 90) {
				if(b.proc.now.lean_forward < lo)
					lo = b.proc.now.lean_forward;
				if(b.proc.now.lean_forward > hi)
					hi = b.proc.now.lean_forward;
			}
		}

		printf("       lean range %.3f .. %.3f rad\n", lo, hi);
		CHECK(hi - lo < 0.05f, "the lean stays within about 3 degrees of steady");
	}

	printf("\nTEST 20: standing about 8 to 12 seconds plays the idle variation\n");
	{
		CharacterMotion b;
		float bx = 0.0f, by = 0.0f;
		float first, ended;

		character_motion_reset(&b, bx, by, 0.0f);
		first = seconds_until(&b, &bx, &by, CHARACTER_ANIM_FIDGET, 14.0f, NULL);
		printf("       first fidget after %.2f s\n", first);
		CHECK(first >= CHARACTER_FIDGET_MIN_SEC && first <= CHARACTER_FIDGET_MAX_SEC + 0.1f,
		      "the first one comes between 8 and 12 seconds");

		ended = seconds_until(&b, &bx, &by, CHARACTER_ANIM_IDLE, 10.0f, NULL);
		printf("       played for %.2f s\n", ended);
		CHECK(fabsf(ended - DURATIONS[CHARACTER_ANIM_FIDGET] * CHARACTER_FIDGET_CYCLES) < 0.1f,
		      "it plays its clip the set number of times, then idles again");

		first = seconds_until(&b, &bx, &by, CHARACTER_ANIM_FIDGET, 14.0f, NULL);
		printf("       next fidget %.2f s after that\n", first);
		CHECK(first >= CHARACTER_FIDGET_MIN_SEC && first <= CHARACTER_FIDGET_MAX_SEC + 0.1f,
		      "the wait restarts when it ends, and is 8 to 12 seconds again");
	}

	printf("\nTEST 21: the wait starts when the body stops moving\n");
	{
		CharacterMotion b;
		float bx = 0.0f, by = 0.0f, t;

		character_motion_reset(&b, bx, by, 0.0f);
		run_for(&b, &bx, &by, 0.0f, 150.0f, 20.0f, NULL);
		CHECK(b.anim != CHARACTER_ANIM_FIDGET, "a body that keeps moving never fidgets");
		t = seconds_until(&b, &bx, &by, CHARACTER_ANIM_FIDGET, 14.0f, NULL);
		CHECK(t >= CHARACTER_FIDGET_MIN_SEC - 1.0f, "the wait counts from when it stopped");

		// start again, move just before it would fidget, and the wait starts over
		character_motion_reset(&b, bx, by, 0.0f);
		run_for(&b, &bx, &by, 0.0f, 0.0f, CHARACTER_FIDGET_MIN_SEC - 0.5f, NULL);
		run_for(&b, &bx, &by, 0.0f, 150.0f, 1.0f, NULL);
		run_for(&b, &bx, &by, 0.0f, 0.0f, 2.0f, NULL);
		CHECK(b.anim != CHARACTER_ANIM_FIDGET, "moving restarts the wait");
	}

	printf("\nTEST 22: moving breaks the fidget off\n");
	{
		CharacterMotion b;
		float bx = 0.0f, by = 0.0f;

		character_motion_reset(&b, bx, by, 0.0f);
		seconds_until(&b, &bx, &by, CHARACTER_ANIM_FIDGET, 14.0f, NULL);
		run_for(&b, &bx, &by, 0.0f, 100.0f, 0.5f, NULL);
		CHECK(b.anim != CHARACTER_ANIM_FIDGET && b.anim != CHARACTER_ANIM_IDLE,
		      "walking off leaves the idle variation straight away");
	}

	printf("\nTEST 23: talking, a seat, a pivot or an aim are not standing about\n");
	{
		CharacterMotion b;
		CharacterHints hints = { 0 };
		float bx = 0.0f, by = 0.0f;

		hints.talking = 1;
		character_motion_reset(&b, bx, by, 0.0f);
		CHECK(seconds_until(&b, &bx, &by, CHARACTER_ANIM_FIDGET, 30.0f, &hints) < 0.0f,
		      "no fidget while in conversation");

		memset(&hints, 0, sizeof(hints));
		hints.has_pose = 1;
		hints.pose = CHARACTER_ANIM_SIT_IDLE;
		character_motion_reset(&b, bx, by, 0.0f);
		CHECK(seconds_until(&b, &bx, &by, CHARACTER_ANIM_FIDGET, 30.0f, &hints) < 0.0f,
		      "none while seated or dancing");

		memset(&hints, 0, sizeof(hints));
		hints.pivoting = 1;
		character_motion_reset(&b, bx, by, 0.0f);
		CHECK(seconds_until(&b, &bx, &by, CHARACTER_ANIM_FIDGET, 30.0f, &hints) < 0.0f,
		      "none while turning on the spot");

		memset(&hints, 0, sizeof(hints));
		hints.has_aim = 1;
		hints.aim_x = 50.0f;
		character_motion_reset(&b, bx, by, 0.0f);
		CHECK(seconds_until(&b, &bx, &by, CHARACTER_ANIM_FIDGET, 30.0f, &hints) < 0.0f,
		      "none while aiming");
	}

	printf("\nTEST 24: every body picks its own delay, always in range\n");
	{
		CharacterMotion b;
		float lowest = 99.0f, highest = 0.0f;

		for(int i = 0; i < 300; i++) {
			character_motion_reset(&b, 0.0f, 0.0f, 0.0f);
			if(b.fidget_after < lowest)
				lowest = b.fidget_after;
			if(b.fidget_after > highest)
				highest = b.fidget_after;
		}
		printf("       delays ranged %.2f .. %.2f s over 300 bodies\n", lowest, highest);
		CHECK(lowest >= CHARACTER_FIDGET_MIN_SEC && highest <= CHARACTER_FIDGET_MAX_SEC,
		      "all within 8 to 12 seconds");
		CHECK(highest - lowest > 2.0f, "and spread across it, not all the same");
	}

	printf("\nTEST 25: with no clip for it, a body just stands\n");
	{
		CharacterMotion b;
		float none[CHARACTER_ANIM_COUNT];
		float bx = 0.0f, by = 0.0f;

		memcpy(none, DURATIONS, sizeof(none));
		none[CHARACTER_ANIM_FIDGET] = 0.0f;
		character_motion_reset(&b, bx, by, 0.0f);
		for(int i = 0; i < 60 * 30; i++)
			character_motion_update(&b, bx, by, DT, NULL, none);
		CHECK(b.anim == CHARACTER_ANIM_IDLE, "still idle after 30 seconds");
	}

	printf("\nTEST 25b: a speed the server reports is used as it is\n");
	{
		CharacterMotion r;
		CharacterHints hints = { 0 };
		float rx = 0.0f, ry = 0.0f;

		hints.has_speed = 1;
		hints.speed = 150.0f;
		character_motion_reset(&r, rx, ry, 0.0f);
		for(int i = 0; i < 6; i++) {
			ry += 150.0f * DT;
			character_motion_update(&r, rx, ry, DT, &hints, DURATIONS);
		}
		CHECK(r.anim == CHARACTER_ANIM_RUN || r.anim == CHARACTER_ANIM_JOG,
		      "moving at the reported speed plays a gait at once");
		hints.speed = 0.0f;
		character_motion_update(&r, rx, ry, DT, &hints, DURATIONS);
		CHECK(r.anim == CHARACTER_ANIM_IDLE, "a reported stop is idle on the next frame");
	}

	printf("\nTEST 25c: a body turns to the facing it is told\n");
	{
		CharacterMotion r;
		CharacterHints hints = { 0 };

		hints.has_heading = 1;
		hints.heading = 2.0f;
		character_motion_reset(&r, 0.0f, 0.0f, 0.0f);
		character_motion_update(&r, 0.0f, 0.0f, DT, &hints, DURATIONS);
		CHECK(angle_between(r.yaw, 2.0f) < 0.001f, "the first facing told is taken as it is");
		CHECK(r.anim == CHARACTER_ANIM_IDLE, "and is no turn to play");

		// a wide turn from a stand is a pivot, a small one is not
		hints.heading = -1.2f;
		character_motion_update(&r, 0.0f, 0.0f, DT, &hints, DURATIONS);
		CHECK(r.anim == CHARACTER_ANIM_PIVOT_LEFT || r.anim == CHARACTER_ANIM_PIVOT_RIGHT,
		      "a wide turn on the spot plays a pivot");
		for(int i = 0; i < 60; i++)
			character_motion_update(&r, 0.0f, 0.0f, DT, &hints, DURATIONS);
		CHECK(angle_between(r.yaw, -1.2f) < 0.001f, "it ends facing the way told");
		CHECK(r.anim == CHARACTER_ANIM_IDLE, "and goes back to standing");

		hints.heading = -1.0f;
		character_motion_update(&r, 0.0f, 0.0f, DT, &hints, DURATIONS);
		CHECK(r.anim == CHARACTER_ANIM_IDLE, "a small turn is only a turn of the body");
	}

	printf("\nTEST 25d: another body's idle variation plays when its owner's does\n");
	{
		CharacterMotion r;
		CharacterHints hints = { 0 };

		hints.has_idle_state = 1;
		character_motion_reset(&r, 0.0f, 0.0f, 0.0f);
		for(int i = 0; i < 60 * 30; i++)
			character_motion_update(&r, 0.0f, 0.0f, DT, &hints, DURATIONS);
		CHECK(r.anim == CHARACTER_ANIM_IDLE, "no variation of its own in half a minute");

		hints.idle_fidget = 1;
		character_motion_update(&r, 0.0f, 0.0f, DT, &hints, DURATIONS);
		CHECK(r.anim == CHARACTER_ANIM_FIDGET, "the variation starts the frame the owner says");
		hints.idle_fidget = 0;
		character_motion_update(&r, 0.0f, 0.0f, DT, &hints, DURATIONS);
		CHECK(r.anim == CHARACTER_ANIM_IDLE, "and stops the frame the owner says");
	}

    printf("\n%s: %d failure(s)\n", g_failures ? "FAILED" : "PASSED", g_failures);
	printf("\nTEST 26: a pivot plays the clip for the way the body turns\n");
	{
		CharacterMotion p;
		CharacterHints hints = { 0 };
		float heading = 0.0f;
		float turn = 540.0f * PI_F / 180.0f;
		float px = 0.0f, py = 0.0f;

		hints.has_facing = 1;
		hints.pivoting = 1;
		character_motion_reset(&p, px, py, 0.0f);
		for(int i = 0; i < 12; i++) {
			heading += turn * DT;
			hints.facing_yaw = heading;
			character_motion_update(&p, px, py, DT, &hints, DURATIONS);
		}
		CHECK(p.anim == CHARACTER_ANIM_PIVOT_LEFT, "toward increasing yaw is the left pivot");
		CHECK(p.anim_time > 0.0f && p.anim_time < DURATIONS[CHARACTER_ANIM_PIVOT_LEFT],
		      "and the clip is playing");

		// the turn eases out near the end: the pivot must not flip or drop early
		for(int i = 0; i < 8; i++) {
			heading += turn * DT * 0.05f;
			hints.facing_yaw = heading;
			character_motion_update(&p, px, py, DT, &hints, DURATIONS);
		}
		CHECK(p.anim == CHARACTER_ANIM_PIVOT_LEFT, "a slow end to the turn keeps the left pivot");

		// keys held still: the facing stops, the pivot still reads as the same turn
		for(int i = 0; i < 4; i++)
			character_motion_update(&p, px, py, DT, &hints, DURATIONS);
		CHECK(p.anim == CHARACTER_ANIM_PIVOT_LEFT, "a frame with no turn does not drop the pivot");

		hints.pivoting = 0;
		run_for(&p, &px, &py, 0.0f, 0.0f, 0.5f, &hints);
		CHECK(p.anim == CHARACTER_ANIM_IDLE, "the pivot ends in idle");

		hints.pivoting = 1;
		character_motion_reset(&p, px, py, heading);
		for(int i = 0; i < 12; i++) {
			heading -= turn * DT;
			hints.facing_yaw = heading;
			character_motion_update(&p, px, py, DT, &hints, DURATIONS);
		}
		CHECK(p.anim == CHARACTER_ANIM_PIVOT_RIGHT, "toward decreasing yaw is the right pivot");

		// the body reverses mid-pivot: the clip follows the new turn
		for(int i = 0; i < 12; i++) {
			heading += turn * DT;
			hints.facing_yaw = heading;
			character_motion_update(&p, px, py, DT, &hints, DURATIONS);
		}
		CHECK(p.anim == CHARACTER_ANIM_PIVOT_LEFT, "a reversed turn switches clips");
	}

	printf("\nTEST 27: speed and other states win over a pivot\n");
	{
		CharacterMotion p;
		CharacterHints hints = { 0 };
		float heading = 0.0f;
		float px = 0.0f, py = 0.0f;

		hints.has_facing = 1;
		hints.pivoting = 1;
		character_motion_reset(&p, px, py, 0.0f);
		p.speed = CHARACTER_JOG_THRESHOLD + 60.0f;
		heading += 0.1f;
		hints.facing_yaw = heading;
		character_motion_update(&p, px, py, DT, &hints, DURATIONS);
		CHECK(p.anim != CHARACTER_ANIM_PIVOT_LEFT && p.anim != CHARACTER_ANIM_PIVOT_RIGHT,
		      "a body still running does not shuffle its feet");

		// no hint, no pivot: another player's body is never in a pivot clip
		character_motion_reset(&p, px, py, 0.0f);
		memset(&hints, 0, sizeof(hints));
		for(int i = 0; i < 12; i++) {
			heading += 0.2f;
			hints.has_facing = 1;
			hints.facing_yaw = heading;
			character_motion_update(&p, px, py, DT, &hints, DURATIONS);
		}
		CHECK(p.anim == CHARACTER_ANIM_IDLE, "turning without the pivot hint stays idle");

		// a seat or a dance is held: the pivot flag does not override it
		memset(&hints, 0, sizeof(hints));
		hints.pivoting = 1;
		hints.has_pose = 1;
		hints.pose = CHARACTER_ANIM_SIT_IDLE;
		character_motion_reset(&p, px, py, 0.0f);
		character_motion_update(&p, px, py, DT, &hints, DURATIONS);
		CHECK(p.anim == CHARACTER_ANIM_SIT_IDLE, "a held pose beats the pivot");
	}

	printf("\nTEST 40: a body told to stand still holds the first frame of idle\n");
	{
		CharacterMotion b;
		CharacterHints still = { 0 };
		float bx = 0.0f, by = 0.0f;

		still.still_idle = 1;
		character_motion_reset(&b, bx, by, 0.0f);

		// well past the longest fidget wait: no clock runs, no variation plays
		run_for(&b, &bx, &by, 0.0f, 0.0f, CHARACTER_FIDGET_MAX_SEC + 5.0f, &still);
		CHECK(b.anim == CHARACTER_ANIM_IDLE && b.anim_time == 0.0f,
		      "it stands on the first frame and never fidgets");

		// the same wait unhinted does fidget, so the hint is what held it
		character_motion_reset(&b, bx, by, 0.0f);
		CHECK(seconds_until(&b, &bx, &by, CHARACTER_ANIM_FIDGET, 14.0f, NULL) < 13.0f,
		      "a body without the hint still fidgets");

		// moving, talking and holding a pose are not idle, so they still play
		character_motion_reset(&b, bx, by, 0.0f);
		run_for(&b, &bx, &by, 0.0f, 150.0f, 1.0f, &still);
		CHECK(b.anim != CHARACTER_ANIM_IDLE && b.anim_time > 0.0f, "it still walks");
		character_motion_reset(&b, bx, by, 0.0f);
		still.talking = 1;
		run_for(&b, &bx, &by, 0.0f, 0.0f, 1.0f, &still);
		CHECK(b.anim == CHARACTER_ANIM_TALK && b.anim_time > 0.0f, "it still talks");
		still.talking = 0;
		still.has_pose = 1;
		still.pose = CHARACTER_ANIM_SIT_IDLE;
		character_motion_reset(&b, bx, by, 0.0f);
		run_for(&b, &bx, &by, 0.0f, 0.0f, 1.0f, &still);
		CHECK(b.anim == CHARACTER_ANIM_SIT_IDLE && b.anim_time > 0.0f, "a seated body still sits");
	}

	printf("\nTEST 41: the idle variations take turns, and another body plays the one named\n");
	{
		float all[CHARACTER_ANIM_COUNT];
		CharacterMotion b;
		CharacterHints hints = { 0 };
		int seen[CHARACTER_ANIM_COUNT] = { 0 };
		int repeats = 0, starts = 0, kind_ok = 1, start_frame = 0, was = 0;
		float glance_len = 0.0f, fold_len = 0.0f;
		CharacterAnim last = CHARACTER_ANIM_IDLE, prev_fidget = CHARACTER_ANIM_COUNT;

		memcpy(all, DURATIONS, sizeof(all));
		all[CHARACTER_ANIM_FIDGET_GLANCE] = 3.5f;
		all[CHARACTER_ANIM_FIDGET_KNUCKLES] = 4.3f;
		all[CHARACTER_ANIM_FIDGET_SLEEVE] = 3.7f;

		character_motion_reset(&b, 0.0f, 0.0f, 0.0f);
		for(int i = 0; i < 60 * 60 * 10; i++) {
			character_motion_update(&b, 0.0f, 0.0f, DT, NULL, all);
			if(b.fidgeting && last == CHARACTER_ANIM_IDLE && b.anim != CHARACTER_ANIM_IDLE) {
				starts++;
				seen[b.anim] = 1;
				repeats += b.anim == prev_fidget;
				prev_fidget = b.anim;
				kind_ok &= b.fidget_kind >= 0 && b.fidget_kind < 4;
				start_frame = i;
			}
			if(was && !b.fidgeting) {
				if(prev_fidget == CHARACTER_ANIM_FIDGET_GLANCE)
					glance_len = (i - start_frame) * DT;
				if(prev_fidget == CHARACTER_ANIM_FIDGET)
					fold_len = (i - start_frame) * DT;
			}
			was = b.fidgeting;
			last = b.anim;
		}
		CHECK(starts > 20, "a body standing ten minutes fidgets many times");
		CHECK(seen[CHARACTER_ANIM_FIDGET] && seen[CHARACTER_ANIM_FIDGET_GLANCE] &&
		      seen[CHARACTER_ANIM_FIDGET_KNUCKLES] && seen[CHARACTER_ANIM_FIDGET_SLEEVE],
		      "all four variations come up");
		CHECK(repeats == 0, "never the same one twice in a row");
		CHECK(kind_ok, "the kind it reports is in range");
		CHECK(fabsf(glance_len - 3.5f) < 0.1f, "a one-gesture variation plays once");
		CHECK(fabsf(fold_len - 2.5f * CHARACTER_FIDGET_CYCLES) < 0.1f,
		      "the arms-folded one still plays its cycles");

		hints.has_idle_state = 1;
		character_motion_reset(&b, 0.0f, 0.0f, 0.0f);
		hints.idle_fidget = 1 + 2;
		character_motion_update(&b, 0.0f, 0.0f, DT, &hints, all);
		CHECK(b.anim == CHARACTER_ANIM_FIDGET_KNUCKLES, "another body plays the variation its owner names");
		hints.idle_fidget = 1 + 7;
		character_motion_update(&b, 0.0f, 0.0f, DT, &hints, all);
		CHECK(b.anim == CHARACTER_ANIM_FIDGET, "an unknown one falls back to the first");
		hints.idle_fidget = 1 + 1;
		character_motion_update(&b, 0.0f, 0.0f, DT, &hints, DURATIONS);
		CHECK(b.anim == CHARACTER_ANIM_FIDGET, "and so does one whose clip is missing");
		hints.idle_fidget = 0;
		character_motion_update(&b, 0.0f, 0.0f, DT, &hints, all);
		CHECK(b.anim == CHARACTER_ANIM_IDLE, "and it stops when the owner stops");
	}

	printf("\n%s: %d failure(s)\n", g_failures ? "FAILED" : "PASSED", g_failures);
    return g_failures ? 1 : 0;
}
