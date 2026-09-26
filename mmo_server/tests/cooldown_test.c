/**
 * @file
 * Exercise the cooldown model: absolute expiry instants, shared across both forms.
 *
 * The design decision under test is that a slot stores when it becomes ready rather
 * than how long is left. Two consequences follow and both are checked here: a
 * cooldown keeps running while the player is in the other form, and no sequence of
 * swaps produces a shorter wait than never swapping at all.
 */

#include "player_effects.h"
#include "progression.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int g_failures = 0;

/** Record one assertion result and print it. */
static void check(int condition, const char* what) {
    if (condition) {
        printf("  ok   %s\n", what);
    } else {
        printf("  FAIL %s\n", what);
        g_failures++;
    }
}

/** Compare two times within a tolerance far tighter than a server tick. */
static int near(double a, double b) {
    return fabs(a - b) < 1e-6;
}

/** Produce a player in Animal Form with nothing on cooldown. */
static void reset_player(ActivePlayer* player) {
    memset(player, 0, sizeof(*player));
    player->character_id = 1;
    player->level = 1;
    player->form = FORM_ANIMAL;
    player->ability_count[FORM_ANIMAL] = MAX_ABILITY_SLOTS;
    player->ability_count[FORM_HUMAN]  = MAX_ABILITY_SLOTS;
}

/** Verify a cooldown counts down from the instant it was started. */
static void test_basic_countdown(void) {
    printf("TEST 1: a cooldown counts down from when it started\n");

    ActivePlayer player;
    reset_player(&player);

    double t0 = 1000.0;
    check(player_slot_is_ready(&player, FORM_ANIMAL, 0, t0), "an untouched slot is ready");
    check(near(player_cooldown_remaining(&player, FORM_ANIMAL, 0, t0), 0.0),
          "and reports no time remaining");

    player_start_cooldown(&player, FORM_ANIMAL, 0, t0, 5.0);

    check(!player_slot_is_ready(&player, FORM_ANIMAL, 0, t0), "starting it makes the slot busy");
    check(near(player_cooldown_remaining(&player, FORM_ANIMAL, 0, t0), 5.0), "with 5s remaining");
    check(near(player_cooldown_remaining(&player, FORM_ANIMAL, 0, t0 + 2.0), 3.0),
          "2s later, 3s remain");
    check(near(player_cooldown_remaining(&player, FORM_ANIMAL, 0, t0 + 5.0), 0.0),
          "at 5s it reads zero");
    check(player_slot_is_ready(&player, FORM_ANIMAL, 0, t0 + 5.0), "and the slot is ready");
    check(player_slot_is_ready(&player, FORM_ANIMAL, 0, t0 + 500.0),
          "and stays ready long afterward");
}

/**
 * Verify the design's worked example: 5s cooldown, swap out for 4s, swap back.
 *
 * The remaining time must be 1s. Nothing in the swap path touches the cooldown, so
 * this is really a check that nothing needs to.
 */
static void test_cooldowns_tick_in_both_forms(void) {
    printf("TEST 2: a cooldown keeps running while the player is in the other form\n");

    ActivePlayer player;
    reset_player(&player);

    double t0 = 500.0;
    player_start_cooldown(&player, FORM_ANIMAL, 2, t0, 5.0);

    /* Swap to Human Form. A form swap changes which row is live and nothing else. */
    player.form = FORM_HUMAN;
    check(player_slot_is_ready(&player, FORM_HUMAN, 2, t0),
          "the same slot on the human bar is independent, and ready");

    /* Four seconds pass in Human Form, then swap back. */
    player.form = FORM_ANIMAL;
    check(near(player_cooldown_remaining(&player, FORM_ANIMAL, 2, t0 + 4.0), 1.0),
          "swapping out for 4s and back leaves exactly 1s remaining");
    check(!player_slot_is_ready(&player, FORM_ANIMAL, 2, t0 + 4.0),
          "so the ability is still not castable");
    check(player_slot_is_ready(&player, FORM_ANIMAL, 2, t0 + 5.0),
          "and becomes castable at the same instant it would have anyway");
}

/**
 * Verify no swap sequence beats not swapping.
 *
 * Any number of swaps at any times must leave the same readiness as a player who
 * never swapped, because the swap path never writes to a cooldown.
 */
static void test_no_swap_sequence_is_faster(void) {
    printf("TEST 3: no swap sequence shortens a cooldown\n");

    ActivePlayer swapper, patient;
    reset_player(&swapper);
    reset_player(&patient);

    double t0 = 100.0;
    player_start_cooldown(&swapper, FORM_ANIMAL, 0, t0, 8.0);
    player_start_cooldown(&patient, FORM_ANIMAL, 0, t0, 8.0);

    /* Thrash the form as hard as a client could. */
    int mismatches = 0;
    for (int i = 1; i <= 40; i++) {
        double now = t0 + i * 0.25;
        swapper.form = (i % 2) ? FORM_HUMAN : FORM_ANIMAL;

        double a = player_cooldown_remaining(&swapper, FORM_ANIMAL, 0, now);
        double b = player_cooldown_remaining(&patient, FORM_ANIMAL, 0, now);
        if (!near(a, b)) mismatches++;
    }
    check(mismatches == 0, "40 swaps over the cooldown's life change nothing");
    check(!player_slot_is_ready(&swapper, FORM_ANIMAL, 0, t0 + 7.99),
          "the swapper is still waiting at 7.99s");
    check(player_slot_is_ready(&swapper, FORM_ANIMAL, 0, t0 + 8.0),
          "and ready at 8.0s, exactly like the patient one");
}

/** Verify the two forms' bars hold independent cooldowns. */
static void test_forms_are_independent(void) {
    printf("TEST 4: the two bars hold separate cooldowns\n");

    ActivePlayer player;
    reset_player(&player);

    double t0 = 0.0;
    player_start_cooldown(&player, FORM_ANIMAL, 1, t0, 10.0);
    player_start_cooldown(&player, FORM_HUMAN,  1, t0, 3.0);

    check(near(player_cooldown_remaining(&player, FORM_ANIMAL, 1, t0), 10.0),
          "the animal slot holds its own 10s");
    check(near(player_cooldown_remaining(&player, FORM_HUMAN, 1, t0), 3.0),
          "the human slot holds its own 3s");

    check(player_slot_is_ready(&player, FORM_HUMAN, 1, t0 + 3.0), "the human slot frees first");
    check(!player_slot_is_ready(&player, FORM_ANIMAL, 1, t0 + 3.0), "the animal slot does not");

    int other_slots_touched = 0;
    for (int slot = 0; slot < MAX_ABILITY_SLOTS; slot++) {
        if (slot == 1) continue;
        if (!player_slot_is_ready(&player, FORM_ANIMAL, slot, t0)) other_slots_touched++;
        if (!player_slot_is_ready(&player, FORM_HUMAN, slot, t0)) other_slots_touched++;
    }
    check(other_slots_touched == 0, "starting one cooldown does not touch any other slot");
}

/** Verify out-of-range arguments are refused rather than written past the array. */
static void test_bounds(void) {
    printf("TEST 5: out-of-range forms and slots are refused\n");

    ActivePlayer player;
    reset_player(&player);

    player_start_cooldown(&player, FORM_COUNT, 0, 0.0, 5.0);
    player_start_cooldown(&player, FORM_ANIMAL, MAX_ABILITY_SLOTS, 0.0, 5.0);
    player_start_cooldown(&player, FORM_ANIMAL, -1, 0.0, 5.0);
    player_start_cooldown(NULL, FORM_ANIMAL, 0, 0.0, 5.0);

    int any_set = 0;
    for (int form = 0; form < FORM_COUNT; form++) {
        for (int slot = 0; slot < MAX_ABILITY_SLOTS; slot++) {
            if (player.ability_ready_at[form][slot] != 0.0) any_set++;
        }
    }
    check(any_set == 0, "no out-of-range call wrote into the array");

    check(near(player_cooldown_remaining(&player, FORM_COUNT, 0, 0.0), 0.0),
          "reading an out-of-range form yields zero");
    check(near(player_cooldown_remaining(NULL, FORM_ANIMAL, 0, 0.0), 0.0),
          "reading a NULL player yields zero");
    check(player_slot_is_ready(NULL, FORM_ANIMAL, 0, 0.0), "a NULL player reads as ready");

    /* A negative duration is a data error, not a way to pre-arm an ability. */
    player_start_cooldown(&player, FORM_ANIMAL, 0, 100.0, -50.0);
    check(player_slot_is_ready(&player, FORM_ANIMAL, 0, 100.0),
          "a negative duration leaves the slot ready rather than ready in the past");
}

int main(void) {
    printf("=== cooldown test ===\n\n");

    progression_init(NULL);

    test_basic_countdown();
    test_cooldowns_tick_in_both_forms();
    test_no_swap_sequence_is_faster();
    test_forms_are_independent();
    test_bounds();

    printf("\n");
    if (g_failures) {
        printf("%d ASSERTION(S) FAILED\n", g_failures);
        return 1;
    }
    printf("ALL ASSERTIONS PASSED\n");
    return 0;
}
