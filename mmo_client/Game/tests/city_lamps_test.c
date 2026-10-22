// Check the city's lamp list: it reads the data file and finds the near lamps.
#include "world/city_lamps.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define NEAR(a, b) (fabsf((a) - (b)) < 1.0e-3f)
#define TMP "city_lamps_test_tmp.json"

static void write_file(const char* text) {
	FILE* f = fopen(TMP, "wb");

	assert(f);
	fputs(text, f);
	fclose(f);
}

static void test_lamps_move_from_scene_metres_to_world_units(void) {
	city_lamp lamp;

	write_file("{\"lamps\":[{\"x\":2,\"y\":3,\"z\":-4,\"radius\":10,\"intensity\":0.5}]}");
	assert(city_lamps_load(TMP, 1000.0f, 2000.0f, 20.0f) == 1);
	assert(city_lamps_nearest(0.0f, 0.0f, 4, &lamp) == 1);
	assert(NEAR(lamp.pos[0], 1040.0f));
	assert(NEAR(lamp.pos[1], 60.0f));
	assert(NEAR(lamp.pos[2], 1920.0f));
	assert(NEAR(lamp.radius, 200.0f));
	// Warm: more red than blue, and dimmed by the intensity
	assert(lamp.color[0] > lamp.color[2]);
	assert(NEAR(lamp.color[0], 0.5f));
}

static void test_radius_and_intensity_have_defaults(void) {
	city_lamp lamp;

	write_file("{\"lamps\":[{\"x\":0,\"y\":0,\"z\":0}]}");
	assert(city_lamps_load(TMP, 0.0f, 0.0f, 10.0f) == 1);
	city_lamps_nearest(0.0f, 0.0f, 1, &lamp);
	assert(lamp.radius > 0.0f);
	assert(lamp.color[0] > 0.9f);
}

static void test_nearest_comes_nearest_first_and_respects_max(void) {
	city_lamp out[4];

	write_file("{\"lamps\":[{\"x\":30,\"y\":1,\"z\":0},{\"x\":10,\"y\":1,\"z\":0},"
	           "{\"x\":20,\"y\":1,\"z\":0}]}");
	assert(city_lamps_load(TMP, 0.0f, 0.0f, 1.0f) == 3);
	assert(city_lamps_nearest(0.0f, 0.0f, 4, out) == 3);
	assert(NEAR(out[0].pos[0], 10.0f) && NEAR(out[1].pos[0], 20.0f) && NEAR(out[2].pos[0], 30.0f));
	assert(city_lamps_nearest(0.0f, 0.0f, 2, out) == 2);
	assert(NEAR(out[1].pos[0], 20.0f));
	assert(city_lamps_nearest(29.0f, 0.0f, 1, out) == 1);
	assert(NEAR(out[0].pos[0], 30.0f));
}

static void test_a_missing_file_leaves_no_lamps(void) {
	write_file("{\"lamps\":[{\"x\":0,\"y\":0,\"z\":0}]}");
	assert(city_lamps_load(TMP, 0.0f, 0.0f, 1.0f) == 1);
	assert(city_lamps_load("no_such_file.json", 0.0f, 0.0f, 1.0f) == -1);
	assert(city_lamps_count() == 0);
}

static void test_broken_files_are_refused(void) {
	write_file("not json at all");
	assert(city_lamps_load(TMP, 0.0f, 0.0f, 1.0f) == -1);
	assert(city_lamps_count() == 0);
	write_file("{\"lights\":[]}");
	assert(city_lamps_load(TMP, 0.0f, 0.0f, 1.0f) == -1);
	// A lamp without a position
	write_file("{\"lamps\":[{\"x\":1,\"y\":2}]}");
	assert(city_lamps_load(TMP, 0.0f, 0.0f, 1.0f) == -1);
	assert(city_lamps_count() == 0);
	// A lamp that lights nothing
	write_file("{\"lamps\":[{\"x\":1,\"y\":2,\"z\":3,\"radius\":0}]}");
	assert(city_lamps_load(TMP, 0.0f, 0.0f, 1.0f) == -1);
}

static void test_too_many_lamps_are_refused(void) {
	char text[4096] = "{\"lamps\":[";

	for(int i = 0; i < CITY_LAMPS_MAX + 1; i++)
		strcat(text, i ? ",{\"x\":0,\"y\":0,\"z\":0}" : "{\"x\":0,\"y\":0,\"z\":0}");
	strcat(text, "]}");
	write_file(text);
	assert(city_lamps_load(TMP, 0.0f, 0.0f, 1.0f) == -1);
	assert(city_lamps_count() == 0);
}

static void test_the_shipped_data_file_loads(void) {
	assert(city_lamps_load("Game/data/city_lights.json", 0.0f, 0.0f, 1.0f) == 4);
}

int main(void) {
	test_lamps_move_from_scene_metres_to_world_units();
	test_radius_and_intensity_have_defaults();
	test_nearest_comes_nearest_first_and_respects_max();
	test_a_missing_file_leaves_no_lamps();
	test_broken_files_are_refused();
	test_too_many_lamps_are_refused();
	test_the_shipped_data_file_loads();
	remove(TMP);
	printf("city_lamps_test: all passed\n");
	return 0;
}
