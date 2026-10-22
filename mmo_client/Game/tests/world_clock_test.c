// Check the world clock: it follows the server's time of day between syncs.
#include "world/world_clock.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define DAY_MS 2700000u

static void test_unsynced_reads_zero(void) {
	world_clock_reset();
	assert(!world_clock_synced());
	assert(world_clock_time_ms(50.0) == 0);
	assert(world_clock_hour(50.0) == 0.0f);
	assert(world_clock_fraction(50.0) == 0.0f);
}

static void test_sync_starts_the_clock(void) {
	world_clock_reset();
	assert(world_clock_sync(1000, DAY_MS, 10.0));
	assert(world_clock_synced());
	assert(world_clock_time_ms(10.0) == 1000);
	assert(world_clock_time_ms(11.0) == 2000);
}

static void test_time_wraps_past_midnight(void) {
	world_clock_reset();
	world_clock_sync(DAY_MS - 1000, DAY_MS, 0.0);
	assert(world_clock_time_ms(0.0) == DAY_MS - 1000);
	assert(world_clock_time_ms(2.0) == 1000);
}

static void test_many_days_later_is_still_in_range(void) {
	world_clock_reset();
	world_clock_sync(500, DAY_MS, 0.0);
	// Seven real days
	assert(world_clock_time_ms(7.0 * 86400.0) < DAY_MS);
	assert(world_clock_time_ms(7.0 * 86400.0) == (500 + 7u * 86400u * 1000u) % DAY_MS);
}

static void test_forty_five_minutes_is_one_day(void) {
	world_clock_reset();
	world_clock_sync(0, DAY_MS, 0.0);
	assert(world_clock_time_ms(2700.0) == 0);
	assert(world_clock_time_ms(2699.0) == DAY_MS - 1000);
}

static void test_hour_and_fraction(void) {
	world_clock_reset();
	world_clock_sync(DAY_MS / 2, DAY_MS, 0.0);
	assert(fabsf(world_clock_hour(0.0) - 12.0f) < 0.001f);
	assert(fabsf(world_clock_fraction(0.0) - 0.5f) < 0.001f);
}

static void test_one_game_hour_is_112_and_a_half_seconds(void) {
	world_clock_reset();
	world_clock_sync(0, DAY_MS, 0.0);
	assert(fabsf(world_clock_hour(112.5) - 1.0f) < 0.001f);
	assert(fabsf(world_clock_hour(1125.0) - 10.0f) < 0.001f);
}

static void test_resync_replaces_the_time(void) {
	world_clock_reset();
	world_clock_sync(1000, DAY_MS, 0.0);
	world_clock_sync(900000, DAY_MS, 5.0);
	assert(world_clock_time_ms(5.0) == 900000);
	assert(world_clock_time_ms(6.0) == 901000);
}

static void test_zero_day_length_is_refused(void) {
	world_clock_reset();
	assert(!world_clock_sync(0, 0, 1.0));
	assert(!world_clock_synced());
}

static void test_time_past_the_day_is_refused(void) {
	world_clock_reset();
	assert(!world_clock_sync(DAY_MS, DAY_MS, 1.0));
	assert(!world_clock_synced());
}

static void test_refused_sync_keeps_the_old_time(void) {
	world_clock_reset();
	world_clock_sync(1000, DAY_MS, 0.0);
	assert(!world_clock_sync(5, 0, 3.0));
	assert(world_clock_synced());
	assert(world_clock_time_ms(3.0) == 4000);
}

static void test_local_clock_before_the_sync_does_not_go_backwards(void) {
	world_clock_reset();
	world_clock_sync(1000, DAY_MS, 10.0);
	assert(world_clock_time_ms(9.0) == 1000);
}

static void test_reset_forgets_the_time(void) {
	world_clock_sync(1000, DAY_MS, 0.0);
	world_clock_reset();
	assert(!world_clock_synced());
	assert(world_clock_time_ms(1.0) == 0);
}

// The clock's text at a game time of day, in both formats
static void expect_text(uint32_t time_of_day_ms, const char* hour12, const char* hour24) {
	char text[16];

	world_clock_sync(time_of_day_ms, DAY_MS, 0.0);
	world_clock_text(0.0, 0, text, sizeof(text));
	assert(strcmp(text, hour12) == 0);
	world_clock_text(0.0, 1, text, sizeof(text));
	assert(strcmp(text, hour24) == 0);
}

static void test_text_is_blank_until_synced(void) {
	char text[16];

	world_clock_reset();
	world_clock_text(0.0, 0, text, sizeof(text));
	assert(strcmp(text, "--:--") == 0);
	world_clock_text(0.0, 1, text, sizeof(text));
	assert(strcmp(text, "--:--") == 0);
}

static void test_text_in_twelve_and_twenty_four_hours(void) {
	world_clock_reset();
	expect_text(0, "12:00 AM", "00:00");
	expect_text(DAY_MS / 24 * 1 + 0, "1:00 AM", "01:00");
	expect_text(684375, "6:05 AM", "06:05");
	expect_text(DAY_MS / 2, "12:00 PM", "12:00");
	expect_text(1631250, "2:30 PM", "14:30");
	expect_text(2698125, "11:59 PM", "23:59");
}

static void test_text_follows_the_clock(void) {
	char text[16];

	world_clock_reset();
	world_clock_sync(0, DAY_MS, 0.0);

	// One game hour is 112.5 seconds
	world_clock_text(112.5, 1, text, sizeof(text));
	assert(strcmp(text, "01:00") == 0);

	// Readers with no clock of their own use the time noted each frame
	world_clock_tick(225.0);
	world_clock_text_now(0, text, sizeof(text));
	assert(strcmp(text, "2:00 AM") == 0);
}

static void test_text_fits_a_small_buffer(void) {
	char text[4];

	world_clock_reset();
	world_clock_sync(1631250, DAY_MS, 0.0);
	world_clock_text(0.0, 0, text, sizeof(text));
	assert(strlen(text) == 3);
	world_clock_text(0.0, 0, text, 0);
}

int main(void) {
	test_unsynced_reads_zero();
	test_sync_starts_the_clock();
	test_time_wraps_past_midnight();
	test_many_days_later_is_still_in_range();
	test_forty_five_minutes_is_one_day();
	test_hour_and_fraction();
	test_one_game_hour_is_112_and_a_half_seconds();
	test_resync_replaces_the_time();
	test_zero_day_length_is_refused();
	test_time_past_the_day_is_refused();
	test_refused_sync_keeps_the_old_time();
	test_local_clock_before_the_sync_does_not_go_backwards();
	test_reset_forgets_the_time();
	test_text_is_blank_until_synced();
	test_text_in_twelve_and_twenty_four_hours();
	test_text_follows_the_clock();
	test_text_fits_a_small_buffer();
	printf("world_clock_test: all passed\n");
	return 0;
}
