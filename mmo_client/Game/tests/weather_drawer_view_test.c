// Check the weather drawer's map: it starts on the whole city, zooms and drags
// within limits, and the image costs the same however big the world is

#include "core/game_types.h"
#include "ui/weather_drawer_view.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>

static const weather_bounds CITY = { -1000.0f, -500.0f, 3000.0f, 1500.0f };
static const weather_rect BOX = { 50.0f, 60.0f, 700.0f, 820.0f };

static int near(float a, float b) {
	return fabsf(a - b) < 0.01f;
}

static void test_it_starts_on_the_whole_footprint(void) {
	weather_cam cam = weather_cam_whole(&CITY);
	map_terrain_view view;
	map_projection proj;
	float sx;
	float sy;

	assert(cam.zoom == 1.0f && near(cam.cam_x, 1000.0f) && near(cam.cam_y, 500.0f));
	assert(weather_drawer_view(&CITY, &BOX, &cam, 1.0f, 2048, &view, &proj));

	map_project(&proj, CITY.min_x, CITY.min_y, &sx, &sy);
	assert(sx >= BOX.x && sy >= BOX.y);
	map_project(&proj, CITY.max_x, CITY.max_y, &sx, &sy);
	assert(sx <= BOX.x + BOX.w && sy <= BOX.y + BOX.h);
}

static void test_the_view_is_the_boxes_size_and_what_it_shows(void) {
	weather_cam cam = weather_cam_whole(&CITY);
	map_terrain_view view;
	map_projection proj;

	assert(weather_drawer_view(&CITY, &BOX, &cam, 2.0f, 4096, &view, &proj));
	assert(view.width == 1400 && view.height == 1640);
	assert(near(view.half_w * proj.k, BOX.w * 0.5f) && near(view.half_h * proj.k, BOX.h * 0.5f));
	assert(near(proj.area.x, BOX.x) && near(proj.area.w, BOX.w));
	assert(view.clip_y == MAP_TERRAIN_NO_CLIP);
}

static void test_the_image_is_capped_and_keeps_the_boxes_shape(void) {
	weather_cam cam = weather_cam_whole(&CITY);
	map_terrain_view view;
	map_projection proj;

	assert(weather_drawer_view(&CITY, &BOX, &cam, 4.0f, 1000, &view, &proj));
	assert(view.height == 1000 && view.width == 854);
}

static void test_the_image_costs_the_same_for_a_far_bigger_world(void) {
	weather_bounds huge = { 0.0f, 0.0f, 400000.0f, 200000.0f };
	weather_cam cam = weather_cam_whole(&huge);
	map_terrain_view view;
	map_projection proj;

	assert(weather_drawer_view(&huge, &BOX, &cam, 1.0f, 2048, &view, &proj));
	assert(view.width == 700 && view.height == 820);
}

static void test_zooming_keeps_the_point_under_the_mouse_still(void) {
	weather_cam cam = weather_cam_whole(&CITY);
	map_projection before;
	map_projection after;
	map_terrain_view view;
	float mouse_x = 600.0f;
	float mouse_y = 300.0f;
	float wx;
	float wy;
	float sx;
	float sy;

	assert(weather_drawer_view(&CITY, &BOX, &cam, 1.0f, 2048, &view, &before));
	map_unproject(&before, mouse_x, mouse_y, &wx, &wy);

	weather_drawer_zoom_at(&CITY, &BOX, &cam, 3.0f, mouse_x, mouse_y);
	assert(near(cam.zoom, 3.0f));
	assert(weather_drawer_view(&CITY, &BOX, &cam, 1.0f, 2048, &view, &after));
	map_project(&after, wx, wy, &sx, &sy);
	assert(fabsf(sx - mouse_x) < 0.1f && fabsf(sy - mouse_y) < 0.1f);
}

static void test_the_zoom_stays_between_the_whole_city_and_the_closest(void) {
	weather_cam cam = weather_cam_whole(&CITY);
	float most = weather_drawer_zoom_max(&CITY, &BOX);

	assert(most > 1.0f);
	weather_drawer_zoom_at(&CITY, &BOX, &cam, 1000.0f, 400.0f, 470.0f);
	assert(near(cam.zoom, most));

	weather_drawer_zoom_at(&CITY, &BOX, &cam, 0.00001f, 400.0f, 470.0f);
	assert(near(cam.zoom, 1.0f));

	// The closest is as close as the full map goes: its smallest half-width
	assert(near(weather_drawer_k(&CITY, &BOX, most), BOX.w * 0.5f / map_zoom_half_width(MAP_ZOOM_MAX)));
}

static void test_dragging_moves_the_map_with_the_mouse_and_stops_near_the_city(void) {
	weather_cam cam = weather_cam_whole(&CITY);
	float k;

	cam.zoom = 4.0f;
	k = weather_drawer_k(&CITY, &BOX, cam.zoom);
	weather_drawer_pan(&CITY, &BOX, &cam, 40.0f, -20.0f);
	assert(near(cam.cam_x, 1000.0f - 40.0f / k) && near(cam.cam_y, 500.0f + 20.0f / k));

	// Dragged a world away it stops a margin outside the footprint
	weather_drawer_pan(&CITY, &BOX, &cam, -1.0e7f, -1.0e7f);
	assert(near(cam.cam_x, CITY.max_x + 4000.0f * 0.12f));
	assert(near(cam.cam_y, CITY.max_y + 2000.0f * 0.12f));
}

static void test_a_footprint_with_no_size_has_no_view(void) {
	weather_bounds flat = { 5.0f, 5.0f, 5.0f, 90.0f };
	weather_cam cam = weather_cam_whole(&flat);
	map_terrain_view view;
	map_projection proj;

	assert(!weather_drawer_view(&flat, &BOX, &cam, 1.0f, 2048, &view, &proj));
	assert(weather_drawer_zoom_max(&flat, &BOX) == 1.0f);
}

int main(void) {
	test_it_starts_on_the_whole_footprint();
	test_the_view_is_the_boxes_size_and_what_it_shows();
	test_the_image_is_capped_and_keeps_the_boxes_shape();
	test_the_image_costs_the_same_for_a_far_bigger_world();
	test_zooming_keeps_the_point_under_the_mouse_still();
	test_the_zoom_stays_between_the_whole_city_and_the_closest();
	test_dragging_moves_the_map_with_the_mouse_and_stops_near_the_city();
	test_a_footprint_with_no_size_has_no_view();
	printf("weather_drawer_view_test: all passed\n");
	return 0;
}
