// Movement validation against a synthetic collision map. Covers the three ways
// a client can lie about where it is: impossible coordinates, impossible speed,
// and walking through geometry.
//
// Build:
//   gcc -Wall -Wextra -pthread -Icommon/include -Iworld_server/include
//       -o move_validator_test tests/move_validator_test.c
//       world_server/src/move_validator.c world_server/src/world_collision.c -lm

#include "move_validator.h"
#include "world_collision.h"

#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAP_W       64
#define MAP_H       64
#define TILE_PX     16
#define WALL_TILE_X 50               // solid column, pixels [800, 816)
#define WORLD_PX    (MAP_W * TILE_PX)  // 1024

// Writes a world.dat whose only solid tiles are the column at WALL_TILE_X.
// Layout mirrors the loader in world_collision.c.
static const char* write_world_dat(void) {
    static char path[] = "/tmp/move_validator_world.dat";
    FILE* f = fopen(path, "wb");
    assert(f);

    int32_t header[3] = { MAP_W, MAP_H, TILE_PX };
    assert(fwrite(header, sizeof(int32_t), 3, f) == 3);

    uint8_t tileset_count = 0;
    assert(fwrite(&tileset_count, 1, 1, f) == 1);

    // Four tile layers, contents irrelevant to collision.
    uint16_t* tiles = calloc((size_t)MAP_W * MAP_H * 4, sizeof(uint16_t));
    assert(tiles);
    assert(fwrite(tiles, sizeof(uint16_t), (size_t)MAP_W * MAP_H * 4, f)
           == (size_t)MAP_W * MAP_H * 4);
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

int main(void) {
    assert(world_collision_init(write_world_dat()));

    const float SPEED = 200.0f;              // px/sec
    const float CEILING = SPEED * MOVE_SPEED_TOLERANCE * MOVE_BUDGET_BURST_SECONDS;

    // ------------------------------------------------------------------
    printf("TEST 1: impossible coordinates are refused\n");
    {
        MoveBudget b;
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
                                          bad[i][0], bad[i][1], SPEED, &t1);
            printf("  (%g, %g) -> %s\n", (double)bad[i][0], (double)bad[i][1],
                   move_verdict_name(v));
            assert(v == MOVE_REJECT_COORD);
        }

        // The tile lookup itself must reject them too, without ever converting
        // a non-finite float to int — that conversion is undefined behaviour,
        // and it was the only thing standing between a NaN and the player's
        // stored position. Run this file under -fsanitize=undefined to prove
        // the cast is guarded.
        assert(world_collision_check(NAN, 100.0f) == 1);
        assert(world_collision_check(100.0f, NAN) == 1);
        assert(world_collision_check(INFINITY, 100.0f) == 1);
        assert(world_collision_check(-1e30f, 100.0f) == 1);
        assert(world_collision_check_box(NAN, NAN, MOVE_PLAYER_HALF_SIZE) == 1);
        assert(world_collision_check_box_path(100.0f, 100.0f, NAN, 100.0f,
                                              MOVE_PLAYER_HALF_SIZE) == 1);
        printf("  non-finite tile lookups rejected without a bad cast\n");
    }

    // ------------------------------------------------------------------
    printf("TEST 2: a normal step at a normal cadence is accepted\n");
    {
        MoveBudget b;
        struct timespec t0 = at_ms(0);
        move_budget_reset(&b, &t0);

        float x = 100.0f;
        // 60Hz client moving at SPEED covers ~3.3px per frame.
        for (int frame = 1; frame <= 120; frame++) {
            struct timespec t = at_ms(frame * 1000 / 60);
            float next = x + SPEED / 60.0f;
            MoveVerdict v = move_validate(&b, x, 100.0f, next, 100.0f, SPEED, &t);
            assert(v == MOVE_ACCEPT);
            x = next;
        }
        printf("  120 frames accepted, x=%.1f\n", (double)x);
        assert(x > 490.0f && x < 510.0f);
    }

    // ------------------------------------------------------------------
    printf("TEST 3: a burst of queued packets cannot outrun the budget\n");
    {
        MoveBudget b;
        struct timespec t0 = at_ms(0);
        move_budget_reset(&b, &t0);

        // One second idle banks the maximum allowance, then 300 packets — the
        // movement burst capacity — arrive in the same millisecond.
        struct timespec burst = at_ms(1000);
        float x = 100.0f;
        int accepted = 0, rejected = 0;
        for (int i = 0; i < 300; i++) {
            float next = x + 2.0f;
            if (move_validate(&b, x, 100.0f, next, 100.0f, SPEED, &burst) == MOVE_ACCEPT) {
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

        // The old clamp granted each sub-millisecond packet 0.1s of travel, so
        // the same burst would have covered the full 600px it asked for.
        assert(travelled < 600.0f);
    }

    // ------------------------------------------------------------------
    printf("TEST 4: credit accrues again once time passes\n");
    {
        MoveBudget b;
        struct timespec t0 = at_ms(0);
        move_budget_reset(&b, &t0);

        struct timespec t1 = at_ms(1000);
        assert(move_validate(&b, 100.0f, 100.0f, 100.0f + CEILING, 100.0f,
                             SPEED, &t1) == MOVE_ACCEPT);
        // Budget is spent: an immediate second jump of the same size fails.
        assert(move_validate(&b, 100.0f + CEILING, 100.0f,
                             100.0f + 2 * CEILING, 100.0f, SPEED, &t1) == MOVE_REJECT_SPEED);
        // After another full second it is available again.
        struct timespec t2 = at_ms(2000);
        assert(move_validate(&b, 100.0f + CEILING, 100.0f,
                             100.0f + 2 * CEILING, 100.0f, SPEED, &t2) == MOVE_ACCEPT);
        printf("  spend, refuse, refill: ok\n");
    }

    // ------------------------------------------------------------------
    printf("TEST 5: a wall cannot be stepped over in one move\n");
    {
        // Both endpoints are clear of the solid column; only the segment
        // between them crosses it. This is exactly what an endpoint-only
        // collision test misses.
        const float from_x = 760.0f, to_x = 832.0f, y = 300.0f;
        assert(world_collision_check_box(from_x, y, MOVE_PLAYER_HALF_SIZE) == 0);
        assert(world_collision_check_box(to_x,   y, MOVE_PLAYER_HALF_SIZE) == 0);
        printf("  both endpoints clear, distance=%.1fpx\n", (double)(to_x - from_x));

        MoveBudget b;
        struct timespec t0 = at_ms(0);
        move_budget_reset(&b, &t0);
        struct timespec t1 = at_ms(1000);   // well inside the budget

        MoveVerdict v = move_validate(&b, from_x, y, to_x, y, SPEED, &t1);
        printf("  swept verdict: %s\n", move_verdict_name(v));
        assert(v == MOVE_REJECT_COLLISION);

        // Walking up to the wall but not into it is still allowed.
        assert(move_validate(&b, from_x, y, 783.0f, y, SPEED, &t1) == MOVE_ACCEPT);
    }

    // ------------------------------------------------------------------
    printf("TEST 6: with no map loaded, nothing is walkable\n");
    {
        world_collision_shutdown();
        assert(world_collision_is_loaded() == 0);
        assert(world_coord_is_valid(100.0f, 100.0f) == 0);
        assert(world_collision_check(100.0f, 100.0f) == 1);
        assert(world_collision_check_box_path(100.0f, 100.0f, 110.0f, 100.0f,
                                              MOVE_PLAYER_HALF_SIZE) == 1);

        MoveBudget b;
        struct timespec t0 = at_ms(0);
        move_budget_reset(&b, &t0);
        struct timespec t1 = at_ms(1000);
        assert(move_validate(&b, 100.0f, 100.0f, 110.0f, 100.0f, SPEED, &t1)
               == MOVE_REJECT_COORD);
        printf("  fails closed\n");
    }

    printf("\nAll move validator tests passed\n");
    return 0;
}
