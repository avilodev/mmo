// Load the tile world, the city scene and its baked data on entering the game.

#include "states/world_loader.h"
#include "states/load_queue.h"
#include "states/join_loader.h"
#include "render/city_renderer.h"
#include "world/city_heights.h"
#include "world/footing.h"
#include "world/world.h"
#include "world_seats.h"
#include "core/client_log.h"

#include <GLFW/glfw3.h>
#include <stdio.h>

#define WORLD_DAT_PATH "Game/bin/world.dat"

static load_queue g_queue;
static int g_begun;
static int g_presented;
static double g_step_started;
static char g_error[160];

static int load_world(void* ctx) {
	GameState* game = ctx;

	if(world_init(&game->world, WORLD_DAT_PATH, WORLD_TILE_PX))
		return LOAD_STEP_DONE;
	CLOG_ERROR("[LOAD] Could not read %s; run world_generator to make it", WORLD_DAT_PATH);
	return LOAD_STEP_FAILED;
}

static int load_city(void* ctx) {
	(void)ctx;
	if(city_renderer_init())
		return LOAD_STEP_DONE;
	CLOG_ERROR("[LOAD] The city scene could not be loaded");
	return LOAD_STEP_FAILED;
}

static int load_heights(void* ctx) {
	(void)ctx;
	if(city_heights_load(CITY_HEIGHTS_PATH))
		return LOAD_STEP_DONE;
	CLOG_ERROR("[LOAD] The city wall heights could not be loaded");
	return LOAD_STEP_FAILED;
}

static int load_footing(void* ctx) {
	(void)ctx;
	if(footing_load(FOOTING_PATH))
		return LOAD_STEP_DONE;
	CLOG_ERROR("[LOAD] Cannot read %s", FOOTING_PATH);
	return LOAD_STEP_FAILED;
}

static int load_seats(void* ctx) {
	(void)ctx;
	if(world_seats_load(CITY_SEATS_PATH))
		return LOAD_STEP_DONE;
	CLOG_ERROR("[LOAD] Cannot read %s", CITY_SEATS_PATH);
	return LOAD_STEP_FAILED;
}

void world_loader_begin(GameState* game) {
	load_queue_init(&g_queue);
	join_loader_reset();
	// Every step is required: a character does not enter with anything missing
	load_queue_add(&g_queue, "Reading the world", 2, 1, 0.0, load_world, game);
	load_queue_add(&g_queue, "Building the city", 6, 1, 0.0, load_city, game);
	load_queue_add(&g_queue, "Measuring the walls", 1, 1, 0.0, load_heights, game);
	load_queue_add(&g_queue, "Laying the floors", 1, 1, 0.0, load_footing, game);
	load_queue_add(&g_queue, "Placing the benches", 1, 1, 0.0, load_seats, game);
	join_loader_add_steps(&g_queue, game);
	g_begun = 1;
	g_presented = 0;
	g_step_started = -1.0;
	g_error[0] = '\0';
	CLOG_INFO("[LOAD] Loading the world");
}

int world_loader_active(void) {
	return g_begun && (!load_queue_finished(&g_queue) || load_queue_failed(&g_queue));
}

int world_loader_failed(void) {
	return g_begun && load_queue_failed(&g_queue);
}

// Say how a step ended, once it has
static void log_step_end(const char* label, double now) {
	double ms = (now - g_step_started) * 1000.0;

	if(load_queue_last_timed_out(&g_queue))
		CLOG_WARN("[LOAD] %s: gave up after %.0f ms", label, ms);
	else
		CLOG_INFO("[LOAD] %s: %.0f ms", label, ms);
}

void world_loader_update(void) {
	const char* label;
	double now;
	int before;

	if(!g_begun || load_queue_finished(&g_queue))
		return;
	// Draw the empty bar once before the first step blocks
	if(!g_presented) {
		g_presented = 1;
		return;
	}
	label = load_queue_label(&g_queue);
	before = g_queue.next;
	now = glfwGetTime();
	if(g_step_started < 0.0)
		g_step_started = now;
	load_queue_step(&g_queue, now);
	if(g_queue.next == before)
		return;
	log_step_end(label, glfwGetTime());
	g_step_started = -1.0;
	if(!load_queue_finished(&g_queue))
		return;
	if(load_queue_failed(&g_queue)) {
		snprintf(g_error, sizeof(g_error), "%s %s. See Game/logs/client.log.",
		         load_queue_last_timed_out(&g_queue) ? "Timed out:" : "Could not finish:",
		         load_queue_label(&g_queue));
		CLOG_ERROR("[LOAD] Stopped at: %s", load_queue_label(&g_queue));
	} else {
		CLOG_INFO("[LOAD] Ready");
	}
}

float world_loader_progress(void) {
	return load_queue_progress(&g_queue);
}

const char* world_loader_label(void) {
	return load_queue_label(&g_queue);
}

const char* world_loader_error(void) {
	return g_error;
}

void world_loader_unload(GameState* game) {
	world_cleanup(&game->world);
	city_renderer_shutdown();
	city_heights_free();
	footing_free();
	world_seats_free();
	load_queue_init(&g_queue);
	join_loader_reset();
	g_begun = 0;
	g_presented = 0;
}
