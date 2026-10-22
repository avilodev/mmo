/**
 * @file
 * Check coordinate, speed-budget, and swept-collision movement rejection.
 */

#define _POSIX_C_SOURCE 200809L

#include "move_validator.h"
#include "world_collision.h"
#include "world_format.h"
#include "world_motion.h"
#include "world_spans_fixture.h"

#include <assert.h>
#include <sys/stat.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** Define the synthetic collision map geometry in tiles and pixels. */
#define MAP_W       64
#define MAP_H       64
#define TILE_PX     16
#define WALL_TILE_X 50               // solid column, pixels [800, 816)
#define SPANS_DIR   "/tmp/move_validator_test_dir/"
#define SPANS_PATH SPANS_DIR "world_spans.bin"
#define WORLD_PX    (MAP_W * TILE_PX)  // 1024

/**
 * Write a collision fixture containing one solid tile column.
 *
 * @return      A static buffer containing the fixture path.
 */
static const char* write_world_dat(void) {
    static char path[] = "/tmp/move_validator_world.dat";
    FILE* f = fopen(path, "wb");
    assert(f);

    /* The versioned preamble, written the way the generator writes it. The
     * fixture must be a real world file: the loader refuses anything without
     * the magic, which is the whole point of the magic. */
    assert(fwrite(WORLD_FORMAT_MAGIC, 1, WORLD_FORMAT_MAGIC_LEN, f)
           == WORLD_FORMAT_MAGIC_LEN);
    uint32_t version     = WORLD_FORMAT_VERSION;
    uint32_t tile_layers = WORLD_FORMAT_TILE_LAYERS;
    assert(fwrite(&version,     sizeof(uint32_t), 1, f) == 1);
    assert(fwrite(&tile_layers, sizeof(uint32_t), 1, f) == 1);

    int32_t header[3] = { MAP_W, MAP_H, TILE_PX };
    assert(fwrite(header, sizeof(int32_t), 3, f) == 3);

    uint8_t tileset_count = 0;
    assert(fwrite(&tileset_count, 1, 1, f) == 1);

    // Four tile layers, contents irrelevant to collision.
    uint16_t* tiles = calloc((size_t)MAP_W * MAP_H * WORLD_FORMAT_TILE_LAYERS, sizeof(uint16_t));
    assert(tiles);
    assert(fwrite(tiles, sizeof(uint16_t), (size_t)MAP_W * MAP_H * WORLD_FORMAT_TILE_LAYERS, f)
           == (size_t)MAP_W * MAP_H * WORLD_FORMAT_TILE_LAYERS);
    free(tiles);

    uint8_t* collision = calloc((size_t)MAP_W * MAP_H, 1);
    assert(collision);
    for (int ty = 0; ty < MAP_H; ty++)
        collision[ty * MAP_W + WALL_TILE_X] = 1;
    assert(fwrite(collision, 1, (size_t)MAP_W * MAP_H, f) == (size_t)MAP_W * MAP_H);
    free(collision);

    fclose(f);
    return path;
}

static struct timespec at_ms(long ms) {
    struct timespec t = { .tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L };
    return t;
}

// What a client would send after predicting `steps` steps in place.
static float predict_z(vertical_state s, float x, float y, int steps, int jump) {
	for(int i = 0; i < steps; i++)
		assert(vertical_step(&s, x, y, MOVE_PLAYER_HALF_SIZE, i == 0 && jump));
	return s.z;
}

static void test_walk_a_ramp(void) {
	MoveBudget b;
	struct timespec t0 = {100, 0}, t1 = {101, 0};
	vertical_state vs = {0.0f, 0.0f, 1};
	float y = FIX_CX(5);

	fix_write_city(SPANS_PATH);
	assert(world_spans_load(SPANS_PATH));

	// three tiles up the ramp in one second at speed 200
	move_budget_reset(&b, &t0);
	assert(move_validate(&b, FIX_CX(10), y, FIX_CX(13), y, 24.0f, 0, 200.0f,
	                     &t1, &vs) == MOVE_ACCEPT);
	assert(vs.z == 24.0f && vs.grounded);
}

// A short hop can round a corner that the line between its ends clips.
static void test_slide_round_a_corner(void) {
	MoveBudget b;
	struct timespec t0 = {100, 0}, t1 = {100, 50000000}, t2 = {100, 200000000};
	vertical_state vs = {0.0f, 0.0f, 1};

	fix_write_city(SPANS_PATH);
	assert(world_spans_load(SPANS_PATH));

	// tile (24, 5) is solid; the body hugs its south face and clears the east end
	move_budget_reset(&b, &t0);
	assert(move_validate(&b, 404.0f, 102.0f, 407.0f, 97.0f, 0.0f, 0, 200.0f,
	                     &t1, &vs) == MOVE_ACCEPT);
	assert(vs.z == 0.0f && vs.grounded);

	// going through the tile is still refused, along the line or either L
	vs = (vertical_state){0.0f, 0.0f, 1};
	move_budget_reset(&b, &t0);
	assert(move_validate(&b, 372.0f, 88.0f, 412.0f, 88.0f, 0.0f, 0, 200.0f,
	                     &t2, &vs) == MOVE_REJECT_HEIGHT);
	move_budget_reset(&b, &t0);
	assert(move_validate(&b, 376.0f, 88.0f, 412.0f, 90.0f, 0.0f, 0, 200.0f,
	                     &t2, &vs) == MOVE_REJECT_HEIGHT);
	assert(vs.z == 0.0f && vs.grounded);
}

static void test_wrong_height_is_refused(void) {
	MoveBudget b;
	struct timespec t0 = {100, 0}, t1 = {101, 0};
	vertical_state vs = {0.0f, 0.0f, 1};
	vertical_state keep;
	float y = FIX_CX(5);

	// float noise passes, a nine unit lie does not
	move_budget_reset(&b, &t0);
	assert(move_validate(&b, FIX_CX(10), y, FIX_CX(13), y, 24.5f, 0, 200.0f,
	                     &t1, &vs) == MOVE_ACCEPT);
	vs = (vertical_state){0.0f, 0.0f, 1};
	keep = vs;
	move_budget_reset(&b, &t0);
	assert(move_validate(&b, FIX_CX(10), y, FIX_CX(13), y, 33.0f, 0, 200.0f,
	                     &t1, &vs) == MOVE_REJECT_HEIGHT);
	assert(vs.z == keep.z && vs.grounded == keep.grounded);

	// a rise taller than a step is a wall
	vs = (vertical_state){24.0f, 0.0f, 1};
	move_budget_reset(&b, &t0);
	assert(move_validate(&b, FIX_CX(13), y, FIX_CX(14), y, 60.0f, 0, 200.0f,
	                     &t1, &vs) == MOVE_REJECT_HEIGHT);
	assert(vs.z == 24.0f);

	// non-finite z is a bad coordinate, before any arithmetic
	vs = (vertical_state){0.0f, 0.0f, 1};
	move_budget_reset(&b, &t0);
	assert(move_validate(&b, FIX_CX(10), y, FIX_CX(11), y, NAN, 0, 200.0f,
	                     &t1, &vs) == MOVE_REJECT_COORD);
	assert(strcmp(move_verdict_name(MOVE_REJECT_HEIGHT), "height") == 0);
}

static void test_jump_and_jitter(void) {
	MoveBudget b;
	struct timespec t0 = {100, 0};
	struct timespec t1 = {100, 250000000};   // 0.25 s = 15 steps
	vertical_state start = {0.0f, 0.0f, 1};
	vertical_state vs;
	float x = FIX_CX(2), y = FIX_CX(2);
	float on_time = predict_z(start, x, y, 15, 1);

	// a jump the server would also have made
	vs = start;
	move_budget_reset(&b, &t0);
	assert(move_validate(&b, x, y, x, y, on_time, 1, 200.0f, &t1, &vs) == MOVE_ACCEPT);
	assert(!vs.grounded && vs.z > 10.0f);

	// two steps early or late is jitter
	vs = start;
	move_budget_reset(&b, &t0);
	assert(move_validate(&b, x, y, x, y, predict_z(start, x, y, 13, 1), 1, 200.0f,
	                     &t1, &vs) == MOVE_ACCEPT);

	// thirty steps off (a client that says it has already landed), or twenty
	// units higher than any jump reaches, is a cheat
	vs = start;
	move_budget_reset(&b, &t0);
	assert(move_validate(&b, x, y, x, y, predict_z(start, x, y, 45, 1), 1, 200.0f,
	                     &t1, &vs) == MOVE_REJECT_HEIGHT);
	vs = start;
	move_budget_reset(&b, &t0);
	assert(move_validate(&b, x, y, x, y, on_time + 20.0f, 1, 200.0f, &t1, &vs)
	       == MOVE_REJECT_HEIGHT);

	// no jump flag, no jump
	vs = start;
	move_budget_reset(&b, &t0);
	assert(move_validate(&b, x, y, x, y, on_time, 0, 200.0f, &t1, &vs)
	       == MOVE_REJECT_HEIGHT);
}

// With no sidecar the world is flat: z stays 0, but jumping still works.
static void test_flat_world(void) {
	MoveBudget b;
	struct timespec t0 = {100, 0}, t1 = {101, 0};
	vertical_state vs = {0.0f, 0.0f, 1};

	world_spans_init(MAP_W, MAP_H, TILE_PX);
	move_budget_reset(&b, &t0);
	assert(move_validate(&b, 100.0f, 100.0f, 110.0f, 100.0f, 0.0f, 0, 200.0f,
	                     &t1, &vs) == MOVE_ACCEPT);
	move_budget_reset(&b, &t0);
	assert(move_validate(&b, 100.0f, 100.0f, 110.0f, 100.0f, 30.0f, 0, 200.0f,
	                     &t1, &vs) == MOVE_REJECT_HEIGHT);
}

// Claimed height for the player after `n` steps of a jump begun at start.
static float jump_z(vertical_state start, float x, float y, int n) {
	return predict_z(start, x, y, n, 1);
}

static struct timespec at_steps(int n) {
	struct timespec t = {100, (long)((double)n * 1e9 / 60.0)};

	return t;
}

// Packets that carry no time must not buy vertical time.
static void test_zero_time_spam(void) {
	MoveBudget b;
	struct timespec t0 = {100, 0};
	struct timespec t1 = at_steps(3);
	vertical_state start = {0.0f, 0.0f, 1};
	vertical_state vs = start;
	float x = FIX_CX(2), y = FIX_CX(2);
	int accepted = 0;
	int refused = 0;

	move_budget_reset(&b, &t0);
	assert(move_validate(&b, x, y, x, y, jump_z(start, x, y, 3), 1, 200.0f, &t1,
	                     &vs) == MOVE_ACCEPT);
	for(int i = 1; i <= 10; i++) {
		MoveVerdict v = move_validate(&b, x, y, x, y, jump_z(start, x, y, 3 + 5 * i),
		                              0, 200.0f, &t1, &vs);

		if(v == MOVE_ACCEPT)
			accepted++;
		else {
			assert(v == MOVE_REJECT_HEIGHT);
			refused++;
		}
		// no matter the verdicts, the state never runs past the slack
		assert(vs.z <= jump_z(start, x, y, 3 + WORLD_AIR_SLACK_STEPS) + 0.01f);
		assert(b.vdrift <= (float)WORLD_AIR_SLACK_STEPS + 0.01f);
	}
	assert(refused >= 4 && accepted <= 10 - 4);
	assert(b.vdrift <= (float)WORLD_AIR_SLACK_STEPS + 0.01f);
}

// Claiming one step per five elapsed is a slow fall; it runs out of slack.
static void test_slow_fall(void) {
	MoveBudget b;
	struct timespec t0 = {100, 0};
	vertical_state start = {0.0f, 0.0f, 1};
	vertical_state vs = start;
	float x = FIX_CX(2), y = FIX_CX(2);
	int rejected_at = 0;

	move_budget_reset(&b, &t0);
	for(int i = 1; i <= 8 && !rejected_at; i++) {
		struct timespec t = at_steps(5 * i);

		if(move_validate(&b, x, y, x, y, jump_z(start, x, y, i), i == 1, 200.0f, &t,
		                 &vs) != MOVE_ACCEPT)
			rejected_at = i;
	}
	assert(rejected_at >= 2 && rejected_at <= 6);
}

// The server follows the client's own fall, not a state nearer the clock
static void test_a_fall_is_followed_exactly_not_lagged(void) {
	MoveBudget b;
	struct timespec t0 = {100, 0};
	struct timespec t1 = at_steps(4);
	vertical_state client = {100.0f, 0.0f, 0};
	vertical_state vs = client;
	float x = FIX_CX(2), y = FIX_CX(2);

	fix_write_city(SPANS_PATH);
	assert(world_spans_load(SPANS_PATH));

	// five steps for the client where the clock allows four: inside the slack
	for(int i = 0; i < 5; i++)
		assert(vertical_step(&client, x, y, MOVE_PLAYER_HALF_SIZE, 0));
	move_budget_reset(&b, &t0);
	assert(move_validate(&b, x, y, x, y, client.z, 0, 200.0f, &t1, &vs) == MOVE_ACCEPT);
	assert(fabsf(vs.z - client.z) < 0.001f);
	assert(fabsf(vs.vz - client.vz) < 0.001f);
}

// A refused move does not pause the body: it falls where the server holds it
static void test_a_refusal_keeps_falling(void) {
	MoveBudget b;
	struct timespec t0 = {100, 0};
	struct timespec t1 = at_steps(6);
	vertical_state vs = {100.0f, 0.0f, 0};
	vertical_state held;
	float x = FIX_CX(2), y = FIX_CX(2);

	fix_write_city(SPANS_PATH);
	assert(world_spans_load(SPANS_PATH));
	move_budget_reset(&b, &t0);
	// a height nowhere near the fall is refused and leaves vs alone
	assert(move_validate(&b, x, y, x, y, 300.0f, 0, 200.0f, &t1, &vs) == MOVE_REJECT_HEIGHT);
	assert(vs.z == 100.0f);
	held = vs;
	move_hold(&b, x, y, &held);
	// six steps of falling from rest, exactly as the client would predict
	for(int i = 0; i < 6; i++)
		assert(vertical_step(&vs, x, y, MOVE_PLAYER_HALF_SIZE, 0));
	assert(fabsf(held.z - vs.z) < 0.001f && fabsf(held.vz - vs.vz) < 0.001f);
	assert(held.z < 100.0f && !held.grounded);
}

// Stale moves are dropped and held; a wrong echo is trusted after a timeout
static void test_stale_moves_are_dropped(void) {
	MoveBudget b;
	struct timespec t0 = {100, 0};
	vertical_state vs = {0.0f, 0.0f, 1};
	float x = FIX_CX(2), y = FIX_CX(2);
	int dropped = 0;

	fix_write_city(SPANS_PATH);
	assert(world_spans_load(SPANS_PATH));
	move_budget_reset(&b, &t0);
	b.correction_seq = 7;
	assert(move_drop_stale(&b, 7, x, y, 200.0f, &t0, &vs) == 0);
	for(int i = 1; i <= 40; i++) {
		struct timespec t = at_steps(3 * i);

		if(!move_drop_stale(&b, 5, x, y, 200.0f, &t, &vs))
			break;
		dropped++;
	}
	// 50 ms apart, 0.75 s of drops before the echo is taken as current
	assert(dropped >= 14 && dropped <= 16);
	assert(b.correction_seq == 5 && vs.grounded);
}

static void test_legit_streams(void) {
	MoveBudget b;
	struct timespec t0 = {100, 0};
	vertical_state start = {0.0f, 0.0f, 1};
	vertical_state vs = start;
	float x = FIX_CX(2), y = FIX_CX(2);

	// exactly predicted, 3 steps per packet
	move_budget_reset(&b, &t0);
	for(int i = 1; i <= 30; i++) {
		struct timespec t = at_steps(3 * i);

		assert(move_validate(&b, x, y, x, y, jump_z(start, x, y, 3 * i), i == 1,
		                     200.0f, &t, &vs) == MOVE_ACCEPT);
	}

	// one step early then one late, alternating
	vs = start;
	move_budget_reset(&b, &t0);
	for(int i = 1; i <= 30; i++) {
		struct timespec t = at_steps(3 * i);
		int claim = 3 * i + ((i & 1) ? 1 : -1);

		assert(move_validate(&b, x, y, x, y, jump_z(start, x, y, claim), i == 1,
		                     200.0f, &t, &vs) == MOVE_ACCEPT);
		assert(fabsf(b.vdrift) <= (float)WORLD_AIR_SLACK_STEPS);
	}
}

static void test_tiny_packets(void) {
	MoveBudget b;
	struct timespec t0 = {100, 0};
	struct timespec t1 = at_steps(15);
	vertical_state start = {0.0f, 0.0f, 1};
	vertical_state vs = start;
	float x = FIX_CX(2), y = FIX_CX(2);

	move_budget_reset(&b, &t0);
	assert(move_validate(&b, x, y, x, y, jump_z(start, x, y, 15), 1, 200.0f, &t1,
	                     &vs) == MOVE_ACCEPT);
	assert(!vs.grounded);
	for(int i = 1; i <= 40; i++) {
		struct timespec t = {t1.tv_sec, t1.tv_nsec + 5000000L * i};

		if(t.tv_nsec >= 1000000000L) {
			t.tv_sec++;
			t.tv_nsec -= 1000000000L;
		}
		assert(move_validate(&b, x, y, x, y, vs.z, 0, 200.0f, &t, &vs) == MOVE_ACCEPT);
		assert(fabsf(b.vdrift) <= (float)WORLD_AIR_SLACK_STEPS);
	}
}

// Both sides of the slack window, pinned early in a jump where height changes
// fastest; the 8 unit air tolerance hides ~3 steps more on the far side.
static void test_window_edges(void) {
	MoveBudget b;
	struct timespec t0 = {100, 0};
	struct timespec t1 = at_steps(15);
	struct timespec t2 = at_steps(3);
	vertical_state start = {0.0f, 0.0f, 1};
	vertical_state vs;
	float x = FIX_CX(2), y = FIX_CX(2);

	// 17 steps claimed when 15 are expected is jitter
	vs = start;
	move_budget_reset(&b, &t0);
	assert(move_validate(&b, x, y, x, y, jump_z(start, x, y, 17), 1, 200.0f, &t1,
	                     &vs) == MOVE_ACCEPT);

	// 3 expected: the last allowed claim is 3 + slack
	vs = start;
	move_budget_reset(&b, &t0);
	assert(move_validate(&b, x, y, x, y, jump_z(start, x, y, 3 + WORLD_AIR_SLACK_STEPS),
	                     1, 200.0f, &t2, &vs) == MOVE_ACCEPT);
	vs = start;
	move_budget_reset(&b, &t0);
	assert(move_validate(&b, x, y, x, y, jump_z(start, x, y, 3 + 2 * WORLD_AIR_SLACK_STEPS + 3),
	                     1, 200.0f, &t2, &vs) == MOVE_REJECT_HEIGHT);
}

// A body in the air cannot be moved into solid and "recovered" onto a lower storey
static void test_no_dropping_through_a_slab(void) {
	static const fix_record recs[] = { {FIX_T(40, 5), 0, 2}, {FIX_T(41, 5), 2, 2} };
	// tile 40: a lower storey and an upper floor at 80; tile 41: a crate at 110
	static const world_span spans[] = { {0, 40}, {80, FIX_OPEN}, {0, 40}, {110, FIX_OPEN} };
	MoveBudget b;
	struct timespec t0 = {100, 0}, t1 = {100, 100000000};
	vertical_state vs = {91.6f, -50.0f, 0};
	vertical_state keep = vs;
	float y = FIX_CX(5);

	fix_write(SPANS_PATH, FIX_W, FIX_H, FIX_TILE, recs, 2, spans, 4,
	          WORLD_SPANS_VERSION, 0);
	assert(world_spans_load(SPANS_PATH));

	move_budget_reset(&b, &t0);
	assert(move_validate(&b, FIX_CX(40), y, FIX_CX(41), y, 0.0f, 0, 200.0f, &t1, &vs)
	       == MOVE_REJECT_HEIGHT);
	assert(vs.z == keep.z && vs.grounded == keep.grounded);
}

// A body that fell into a ledge where it stood is stood up there, then walks on
static void test_a_stuck_body_is_stood_up_then_moves(void) {
	static const fix_record recs[] = { {FIX_T(41, 5), 0, 2} };
	// tile 41: ground under a slab, and a deck at 106; tile 40 is open ground
	static const world_span spans[] = { {0, 96}, {106, FIX_OPEN} };
	MoveBudget b;
	struct timespec t0 = {100, 0}, t1 = {100, 100000000};
	vertical_state vs = {80.0f, 0.0f, 0};
	float y = FIX_CX(5);

	fix_write(SPANS_PATH, FIX_W, FIX_H, FIX_TILE, recs, 1, spans, 2,
	          WORLD_SPANS_VERSION, 0);
	assert(world_spans_load(SPANS_PATH));
	// inside the slab at the packet's start, then a step away from it
	assert(!vertical_step(&(vertical_state){80.0f, 0.0f, 0}, FIX_CX(40) + 6.0f, y,
	                      MOVE_PLAYER_HALF_SIZE, 0));
	move_budget_reset(&b, &t0);
	assert(move_validate(&b, FIX_CX(40) + 6.0f, y, FIX_CX(40) - 2.0f, y, 0.0f, 0, 200.0f,
	                     &t1, &vs) == MOVE_ACCEPT);
	assert(vs.grounded && vs.z == 0.0f);
}

/**
 * Run movement validation against the synthetic collision map.
 *
 * @return      Zero after all assertions pass.
 */
int main(void) {
    mkdir(SPANS_DIR, 0700);
    assert(world_collision_init(write_world_dat()));
    world_spans_init(MAP_W, MAP_H, TILE_PX);

    const float SPEED = 200.0f;              // px/sec
    const float CEILING = SPEED * MOVE_SPEED_TOLERANCE * MOVE_BUDGET_BURST_SECONDS;

    printf("TEST 1: impossible coordinates are refused\n");
    {
        MoveBudget b;
        vertical_state vs = {0.0f, 0.0f, 1};
        struct timespec t0 = at_ms(0);
        move_budget_reset(&b, &t0);
        struct timespec t1 = at_ms(1000);   // a full second of credit banked

        const float bad[][2] = {
            { NAN, 100.0f }, { 100.0f, NAN },
            { INFINITY, 100.0f }, { -INFINITY, 100.0f },
            { -1.0f, 100.0f },                  // outside the world
            { (float)WORLD_PX, 100.0f },        // exactly at the far edge
            { 1e30f, 100.0f },
        };
        for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
            MoveVerdict v = move_validate(&b, 100.0f, 100.0f,
                                          bad[i][0], bad[i][1], 0.0f, 0, SPEED, &t1, &vs);
            printf("  (%g, %g) -> %s\n", (double)bad[i][0], (double)bad[i][1],
                   move_verdict_name(v));
            assert(v == MOVE_REJECT_COORD);
        }

        // reject non-finite values before tile-index conversion
        assert(world_collision_check(NAN, 100.0f) == 1);
        assert(world_collision_check(100.0f, NAN) == 1);
        assert(world_collision_check(INFINITY, 100.0f) == 1);
        assert(world_collision_check(-1e30f, 100.0f) == 1);
        assert(world_collision_check_box(NAN, NAN, MOVE_PLAYER_HALF_SIZE) == 1);
        assert(world_collision_check_box_path(100.0f, 100.0f, NAN, 100.0f,
                                              MOVE_PLAYER_HALF_SIZE) == 1);
        printf("  non-finite tile lookups rejected without a bad cast\n");
    }

    printf("TEST 2: a normal step at a normal cadence is accepted\n");
    {
        MoveBudget b;
        vertical_state vs = {0.0f, 0.0f, 1};
        struct timespec t0 = at_ms(0);
        move_budget_reset(&b, &t0);

        float x = 100.0f;
        // 60Hz client moving at SPEED covers ~3.3px per frame.
        for (int frame = 1; frame <= 120; frame++) {
            struct timespec t = at_ms(frame * 1000 / 60);
            float next = x + SPEED / 60.0f;
            MoveVerdict v = move_validate(&b, x, 100.0f, next, 100.0f, 0.0f, 0, SPEED, &t, &vs);
            assert(v == MOVE_ACCEPT);
            x = next;
        }
        printf("  120 frames accepted, x=%.1f\n", (double)x);
        assert(x > 490.0f && x < 510.0f);
    }

    printf("TEST 3: a burst of queued packets cannot outrun the budget\n");
    {
        MoveBudget b;
        vertical_state vs = {0.0f, 0.0f, 1};
        struct timespec t0 = at_ms(0);
        move_budget_reset(&b, &t0);

        // submit one full movement burst at one timestamp
        struct timespec burst = at_ms(1000);
        float x = 100.0f;
        int accepted = 0, rejected = 0;
        for (int i = 0; i < 300; i++) {
            float next = x + 2.0f;
            if (move_validate(&b, x, 100.0f, next, 100.0f, 0.0f, 0, SPEED, &burst, &vs) == MOVE_ACCEPT) {
                x = next;
                accepted++;
            } else {
                rejected++;
            }
        }
        float travelled = x - 100.0f;
        printf("  accepted=%d rejected=%d travelled=%.1fpx (ceiling %.1f)\n",
               accepted, rejected, (double)travelled, (double)CEILING);
        assert(rejected > 0);
        assert(travelled <= CEILING + 0.001f);

        assert(travelled < 600.0f);
    }

    printf("TEST 4: credit accrues again once time passes\n");
    {
        MoveBudget b;
        vertical_state vs = {0.0f, 0.0f, 1};
        struct timespec t0 = at_ms(0);
        move_budget_reset(&b, &t0);

        struct timespec t1 = at_ms(1000);
        assert(move_validate(&b, 100.0f, 100.0f, 100.0f + CEILING, 100.0f,
                             0.0f, 0, SPEED, &t1, &vs) == MOVE_ACCEPT);
        // Budget is spent: an immediate second jump of the same size fails.
        assert(move_validate(&b, 100.0f + CEILING, 100.0f,
                             100.0f + 2 * CEILING, 100.0f, 0.0f, 0, SPEED, &t1, &vs) == MOVE_REJECT_SPEED);
        // After another full second it is available again.
        struct timespec t2 = at_ms(2000);
        assert(move_validate(&b, 100.0f + CEILING, 100.0f,
                             100.0f + 2 * CEILING, 100.0f, 0.0f, 0, SPEED, &t2, &vs) == MOVE_ACCEPT);
        printf("  spend, refuse, refill: ok\n");
    }

    printf("TEST 5: a wall cannot be stepped over in one move\n");
    {
        // cross the wall with two clear endpoints
        const float from_x = 760.0f, to_x = 832.0f, y = 300.0f;
        assert(world_collision_check_box(from_x, y, MOVE_PLAYER_HALF_SIZE) == 0);
        assert(world_collision_check_box(to_x,   y, MOVE_PLAYER_HALF_SIZE) == 0);
        printf("  both endpoints clear, distance=%.1fpx\n", (double)(to_x - from_x));

        MoveBudget b;
        vertical_state vs = {0.0f, 0.0f, 1};
        struct timespec t0 = at_ms(0);
        move_budget_reset(&b, &t0);
        struct timespec t1 = at_ms(1000);   // well inside the budget

        MoveVerdict v = move_validate(&b, from_x, y, to_x, y, 0.0f, 0, SPEED, &t1, &vs);
        printf("  swept verdict: %s\n", move_verdict_name(v));
        assert(v == MOVE_REJECT_COLLISION);

        // Walking up to the wall but not into it is still allowed.
        assert(move_validate(&b, from_x, y, 783.0f, y, 0.0f, 0, SPEED, &t1, &vs) == MOVE_ACCEPT);
    }

    printf("TEST 5b: vertical simulation replay\n");
    test_walk_a_ramp();
    test_slide_round_a_corner();
    test_wrong_height_is_refused();
    test_jump_and_jitter();
    test_zero_time_spam();
    test_slow_fall();
    test_a_fall_is_followed_exactly_not_lagged();
	test_a_refusal_keeps_falling();
	test_stale_moves_are_dropped();
    test_legit_streams();
    test_tiny_packets();
    test_window_edges();
    test_no_dropping_through_a_slab();
    test_a_stuck_body_is_stood_up_then_moves();
    test_flat_world();
    printf("  height checks ok\n");

    printf("TEST 6: with no map loaded, nothing is walkable\n");
    {
        world_collision_shutdown();
        assert(world_collision_is_loaded() == 0);
        assert(world_coord_is_valid(100.0f, 100.0f) == 0);
        assert(world_collision_check(100.0f, 100.0f) == 1);
        assert(world_collision_check_box_path(100.0f, 100.0f, 110.0f, 100.0f,
                                              MOVE_PLAYER_HALF_SIZE) == 1);

        MoveBudget b;
        vertical_state vs = {0.0f, 0.0f, 1};
        struct timespec t0 = at_ms(0);
        move_budget_reset(&b, &t0);
        struct timespec t1 = at_ms(1000);
        assert(move_validate(&b, 100.0f, 100.0f, 110.0f, 100.0f, 0.0f, 0, SPEED, &t1, &vs)
               == MOVE_REJECT_COORD);
        printf("  fails closed\n");
    }

    printf("\nAll move validator tests passed\n");
    return 0;
}
