/**
 * @file
 * Check that a server correction is reconciled rather than applied as a jump.
 *
 * The client predicts locally and keeps sending. A refusal describes where the
 * player was when the refused move was sent -- several frames ago by the time
 * it lands. Assigning that position to *now* throws away every step taken in
 * between, and at 20Hz over any latency spike that is a visible jerk even when
 * the server agreed with the direction of travel. Most corrections are of that
 * kind: the server did not disagree, it answered late.
 *
 * So each move carries a sequence, the correction names the one it refuses,
 * and the client rewinds to the server's position and replays what it sent
 * afterwards. The property worth testing is not "the position changed" but
 * *which* position it lands on, and in particular that a correction the client
 * would have agreed with moves it nowhere at all.
 *
 * The world is stubbed to a wall map the test writes, so the replay's
 * collision behaviour is checkable rather than a property of world.dat.
 * Rendering is stubbed the way inventory's tests stub it -- player.c mixes the
 * model with the code that draws it.
 */

#include "player.h"
#include "world/world.h"
#include "core/keybinds.h"
#include "core/client_log.h"

#include <math.h>
#include <stdint.h>
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

/* --- Stubs --------------------------------------------------------------- */

#include "hostcompat/render_stubs.c"

KeyBinds g_keybinds;

int input_key_pressed(const InputState* input, int key) {
    (void)input; (void)key; return 0;
}

int input_key_just_pressed(const InputState* input, int key) {
    (void)input; (void)key; return 0;
}

/** The wall the replay has to notice, as a vertical band in world pixels. */
static float g_wall_min_x = 0.0f;
static float g_wall_max_x = 0.0f;   /* empty band: nothing is solid */

static void wall_none(void)  { g_wall_min_x = g_wall_max_x = 0.0f; }
static void wall_at(float min_x, float max_x) {
    g_wall_min_x = min_x;
    g_wall_max_x = max_x;
}

int world_check_box_collision(const WorldState* world, float x, float y, float half) {
    (void)world; (void)y;
    if (g_wall_max_x <= g_wall_min_x) return 0;
    /* The box overlaps the band. */
    return (x + half) > g_wall_min_x && (x - half) < g_wall_max_x;
}

/* --- Fixture ------------------------------------------------------------- */

#define TILE 16

static WorldState  g_world;
static PlayerState g_player;

static void fresh(float x, float y) {
    memset(&g_world, 0, sizeof(g_world));
    g_world.tile_size = TILE;

    player_init(&g_player);
    g_player.x = x;
    g_player.y = y;
    wall_none();
}

/** Send a move: number it, remember it, and pretend the packet went out. */
static uint32_t send_move(uint32_t sequence) {
    player_record_sent_move(&g_player, sequence, g_player.x, g_player.y);
    return sequence;
}

/** Move the local prediction without touching the sent-move history. */
static void predict(float dx, float dy) {
    g_player.x += dx;
    g_player.y += dy;
}

static int near_enough(float a, float b) { return fabsf(a - b) < 0.01f; }

/* --- Cases --------------------------------------------------------------- */

static void test_an_agreeing_correction_is_invisible(void) {
    printf("a correction that agrees with the prediction moves nothing\n");

    /* The case that matters most, because it is the common one. The client
     * proposes a position at sequence 7 and carries on walking. The server
     * refuses that move but its authoritative position is the *same* place the
     * client thought it was -- the refusal was a speed-budget edge, not a
     * disagreement about where the player is.
     *
     * Applied as a teleport, the player is yanked back the 40px they have
     * walked since. Reconciled, they do not move at all. */
    fresh(1000.0f, 500.0f);

    send_move(7);                  /* claimed (1000, 500) */
    predict(40.0f, 0.0f);          /* four frames of walking east */

    float before_x = g_player.x, before_y = g_player.y;

    int visible = player_apply_correction(&g_player, &g_world, 7, 1000.0f, 500.0f);

    CHECK(!visible, "the correction reports itself as invisible");
    CHECK(near_enough(g_player.x, before_x) && near_enough(g_player.y, before_y),
          "and the player is exactly where they already were");
}

static void test_a_disagreeing_correction_keeps_the_tail(void) {
    printf("a correction that disagrees still keeps what was walked since\n");

    /* The server genuinely disagrees: it puts the player 100px west of where
     * the client claimed at sequence 7. The client has walked 40px east since.
     *
     * The right answer is neither "where the client is" (the server refused
     * that) nor "where the server says" (that was true four frames ago). It is
     * the server's position plus the tail: 100px back, then the 40px walked. */
    fresh(1000.0f, 500.0f);

    send_move(7);
    predict(40.0f, 0.0f);

    int visible = player_apply_correction(&g_player, &g_world, 7, 900.0f, 500.0f);

    CHECK(visible, "the correction reports a visible move");
    CHECK(near_enough(g_player.x, 940.0f),
          "the player lands at the server's position plus the replayed tail");
    CHECK(near_enough(g_player.y, 500.0f), "and did not drift on the other axis");
}

static void test_the_tail_is_measured_from_the_refused_move(void) {
    printf("the refused move's own step is dropped, later ones are not\n");

    /* Three proposals. The server refuses the *first* of them, so the two
     * steps taken after it are the tail -- and the step the refused proposal
     * itself claimed is not, because the server would not accept it.
     *
     * Measuring from the recorded position of the refused sequence is what
     * makes that split fall out for free rather than being counted by hand. */
    fresh(0.0f, 0.0f);

    send_move(1);            /* claimed (0, 0) */
    predict(10.0f, 0.0f);
    send_move(2);            /* claimed (10, 0) */
    predict(10.0f, 0.0f);
    send_move(3);            /* claimed (20, 0) */
    predict(10.0f, 0.0f);    /* now at (30, 0), unsent */

    /* Refusing sequence 2: the server says the player is at (5, 0). The tail
     * after sequence 2 is 30 - 10 = 20px. */
    player_apply_correction(&g_player, &g_world, 2, 5.0f, 0.0f);

    CHECK(near_enough(g_player.x, 25.0f),
          "the tail is everything after the refused proposal, and only that");

    /* Refusing the newest instead leaves no tail at all: the client has walked
     * nowhere since sequence 3 was... except it has. Check the boundary the
     * other way with a fresh run. */
    fresh(0.0f, 0.0f);
    send_move(9);            /* claimed (0, 0) */
    player_apply_correction(&g_player, &g_world, 9, -50.0f, 0.0f);
    CHECK(near_enough(g_player.x, -50.0f),
          "a refusal with nothing sent after it is the server's position exactly");
}

static void test_an_unknown_sequence_falls_back(void) {
    printf("a correction naming a move the ring no longer holds is taken as-is\n");

    /* Correctness before cleverness. A sequence older than the ring, or from
     * before a reconnect, cannot be reconciled against anything -- and the
     * server is authoritative either way, so the fallback is the behaviour
     * this code had for every correction before the ring existed. */
    fresh(1000.0f, 500.0f);
    send_move(7);
    predict(40.0f, 0.0f);

    int visible = player_apply_correction(&g_player, &g_world, 999, 800.0f, 400.0f);

    CHECK(visible, "an unreconcilable correction is reported as visible");
    CHECK(near_enough(g_player.x, 800.0f) && near_enough(g_player.y, 400.0f),
          "and the server's position is taken exactly");

    /* Sequence zero is the "no move" sentinel and must not match an empty ring
     * slot, which would otherwise reconcile against a position of (0, 0). */
    fresh(1000.0f, 500.0f);
    predict(40.0f, 0.0f);
    player_apply_correction(&g_player, &g_world, 0, 700.0f, 300.0f);
    CHECK(near_enough(g_player.x, 700.0f) && near_enough(g_player.y, 300.0f),
          "sequence zero does not match an empty ring slot");
}

static void test_the_ring_wraps_without_confusing_itself(void) {
    printf("the ring holds the newest moves and forgets the oldest\n");

    fresh(0.0f, 0.0f);

    /* Three full turns of the ring. Every position is distinct, so a wrapped
     * slot resolving to the wrong entry would land the replay somewhere
     * arithmetically obvious. */
    const int total = PLAYER_MOVE_HISTORY * 3;
    for (int i = 1; i <= total; i++) {
        send_move((uint32_t)i);
        predict(10.0f, 0.0f);
    }
    /* Now at (total * 10, 0); sequence N claimed ((N - 1) * 10, 0). */

    uint32_t newest = (uint32_t)total;
    player_apply_correction(&g_player, &g_world, newest, 0.0f, 0.0f);
    CHECK(near_enough(g_player.x, 10.0f),
          "the newest sequence resolves to the position it claimed");

    /* Rebuild and check the oldest still in the ring. */
    fresh(0.0f, 0.0f);
    for (int i = 1; i <= total; i++) {
        send_move((uint32_t)i);
        predict(10.0f, 0.0f);
    }
    uint32_t oldest_held = (uint32_t)(total - PLAYER_MOVE_HISTORY + 1);
    float claimed_x = (float)(oldest_held - 1) * 10.0f;
    float tail      = (float)total * 10.0f - claimed_x;
    player_apply_correction(&g_player, &g_world, oldest_held, 0.0f, 0.0f);
    CHECK(near_enough(g_player.x, tail),
          "and so does the oldest the ring still holds");

    /* One older than that is gone, and must not resolve to a wrapped slot. */
    fresh(0.0f, 0.0f);
    for (int i = 1; i <= total; i++) {
        send_move((uint32_t)i);
        predict(10.0f, 0.0f);
    }
    player_apply_correction(&g_player, &g_world,
                            (uint32_t)(total - PLAYER_MOVE_HISTORY), 12345.0f, 0.0f);
    CHECK(near_enough(g_player.x, 12345.0f),
          "one older than the ring holds falls back rather than matching a wrap");
}

static void test_the_replay_does_not_walk_through_walls(void) {
    printf("a replayed tail is walked against the world, not added to it\n");

    /* The tail is several frames of movement in one step, so adding it blindly
     * can cross a wall the player would have been stopped by. The replay walks
     * it in half-tile samples for the same reason the server samples its own
     * path checks. */
    fresh(0.0f, 0.0f);
    send_move(1);
    predict(200.0f, 0.0f);    /* the client walked 200px east, unobstructed */

    /* The server puts the player back at the origin -- and between the origin
     * and 200px east there is now a wall. */
    wall_at(80.0f, 96.0f);

    player_apply_correction(&g_player, &g_world, 1, 0.0f, 0.0f);

    CHECK(g_player.x < 80.0f,
          "the replay stopped at the wall rather than teleporting past it");
    CHECK(g_player.x > 0.0f,
          "but still made the progress it could before reaching it");

    /* And with no wall in the way, the same tail arrives in full -- so the
     * stop above is the wall, not the sampling losing distance. */
    fresh(0.0f, 0.0f);
    send_move(1);
    predict(200.0f, 0.0f);
    wall_none();
    player_apply_correction(&g_player, &g_world, 1, 0.0f, 0.0f);
    CHECK(near_enough(g_player.x, 200.0f),
          "an unobstructed tail of the same length arrives in full");
}

static void test_a_teleport_forgets_the_unanswered_moves(void) {
    printf("a teleport drops the history a replay would be measured against\n");

    /* The ring measures how far the player has walked since a proposal. Across
     * a respawn or a world entry that measurement is nonsense: the tail would
     * include the jump itself, and replaying it would land the player a whole
     * teleport away from anywhere either end asked for.
     *
     * So the history goes, and a correction naming a pre-jump sequence becomes
     * unreconcilable -- which is the honest answer, and lands on the server's
     * position exactly. */
    fresh(1000.0f, 500.0f);
    send_move(4);
    predict(30.0f, 0.0f);

    player_reset_position(&g_player, 20.0f, 20.0f);   /* a respawn across the map */
    predict(10.0f, 0.0f);

    player_apply_correction(&g_player, &g_world, 4, 900.0f, 400.0f);

    CHECK(near_enough(g_player.x, 900.0f) && near_enough(g_player.y, 400.0f),
          "the stale correction is taken as-is rather than replayed across the jump");

    /* And a move sent after the teleport reconciles normally again. */
    fresh(1000.0f, 500.0f);
    send_move(4);
    player_reset_position(&g_player, 20.0f, 20.0f);
    send_move(5);
    predict(15.0f, 0.0f);
    player_apply_correction(&g_player, &g_world, 5, 20.0f, 20.0f);
    CHECK(near_enough(g_player.x, 35.0f),
          "while a move sent after it replays its tail as usual");
}

static void test_a_correction_without_a_world_still_corrects(void) {
    printf("a correction with no world to check against is still applied\n");

    /* Defensive: the reconciliation needs a world to walk the tail against,
     * and a caller that has none must still end up at the server's position
     * rather than wherever it was. */
    fresh(1000.0f, 500.0f);
    send_move(3);
    predict(40.0f, 0.0f);

    player_apply_correction(&g_player, NULL, 3, 111.0f, 222.0f);
    CHECK(near_enough(g_player.x, 111.0f) && near_enough(g_player.y, 222.0f),
          "the server's position is taken without a replay");

    /* And a NULL player is survivable, because a correction can arrive during
     * teardown. */
    player_apply_correction(NULL, &g_world, 1, 0.0f, 0.0f);
    player_record_sent_move(NULL, 1, 0.0f, 0.0f);
    CHECK(1, "a NULL player does not crash either call");
}

int main(void) {
    client_log_init();
    printf("=== player reconciliation ===\n\n");

    test_an_agreeing_correction_is_invisible();
    test_a_disagreeing_correction_keeps_the_tail();
    test_the_tail_is_measured_from_the_refused_move();
    test_an_unknown_sequence_falls_back();
    test_the_ring_wraps_without_confusing_itself();
    test_the_replay_does_not_walk_through_walls();
    test_a_teleport_forgets_the_unanswered_moves();
    test_a_correction_without_a_world_still_corrects();

    client_log_close();

    printf("\n");
    if (g_failures) {
        printf("%d ASSERTION%s FAILED\n", g_failures, g_failures == 1 ? "" : "S");
        return 1;
    }
    printf("ALL ASSERTIONS PASSED\n");
    return 0;
}
