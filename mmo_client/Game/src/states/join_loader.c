// Wait on the loading screen for everything the server sends at join.

#include "states/join_loader.h"
#include "states/join_data.h"
#include "network.h"
#include "network/name_cache.h"
#include "player.h"
#include "inventory.h"
#include "ability_bar.h"
#include "camera/camera.h"
#include "world/world_clock.h"
#include "core/client_log.h"

// Seconds a step may wait for the server before the load fails
#define WAIT_REQUIRED 10.0

static int g_requests_sent;

void join_loader_reset(void) {
	join_data_reset();
	g_requests_sent = 0;
}

void join_apply_character(GameState* game, const CharacterInfo* info) {
	player_load_info(&game->player, info);

	// The view starts on the character, not sweeping to it from wherever it was
	camera_set_position(&game->camera, game->player.x, game->player.y);
	game->camera.z = game->player.visual_z;
	game->camera.distance_limit = 0.0f;

	// Seed the bar with the form and pool the character entered in
	game->playing->ability_bar.active_form = info->form;
	game->playing->ability_bar.resource_type = info->resource_type;
	ability_bar_on_resource_update(&game->playing->ability_bar, (int32_t)info->resource,
	                               (int32_t)info->max_resource);

	// Full stats: move speed, XP to the next level and the rest
	network_request_player_stats();

	if(game->inventory) {
		inventory_load_from_server(game->inventory, info->inventory);
		CLOG_INFO("[GAME] Inventory loaded from server");
	}
}

// What the panels would otherwise ask for when first opened, asked for up front
static void send_requests_once(void) {
	if(g_requests_sent)
		return;
	g_requests_sent = 1;
	network_send_friend_list_request();
	network_send_session_list_request(0);
}

static int wait_for(unsigned flags) {
	return join_data_seen(flags) ? LOAD_STEP_DONE : LOAD_STEP_WAIT;
}

static int step_character(void* ctx) {
	GameState* game = ctx;
	CharacterInfo info;

	send_requests_once();
	if(game->player.info_loaded)
		return LOAD_STEP_DONE;
	if(!network_get_character_data(&info))
		return LOAD_STEP_WAIT;
	join_apply_character(game, &info);
	return LOAD_STEP_DONE;
}

// The clock is kept by world_clock, not join_data: it arrives with the world
// connection, before the loader begins, and join_data is cleared at the start
static int step_time(void* ctx) {
	(void)ctx;
	return world_clock_synced() ? LOAD_STEP_DONE : LOAD_STEP_WAIT;
}

static int step_stats(void* ctx) {
	(void)ctx;
	return wait_for(JOIN_STATS);
}

static int step_abilities(void* ctx) {
	(void)ctx;
	return wait_for(JOIN_ABILITIES);
}

static int step_quests(void* ctx) {
	(void)ctx;
	return wait_for(JOIN_QUESTS);
}

static int step_friends(void* ctx) {
	(void)ctx;
	return wait_for(JOIN_FRIENDS | JOIN_FRIEND_REQUESTS);
}

static int step_online_list(void* ctx) {
	(void)ctx;
	return wait_for(JOIN_ONLINE_LIST);
}

// The server sends every connection its first NPC and player lists, empty or not
static int step_nearby(void* ctx) {
	(void)ctx;
	return wait_for(JOIN_NPCS | JOIN_PLAYERS);
}

// Names of the players in view. One who leaves view is no longer waited for.
static int step_names(void* ctx) {
	GameState* game = ctx;
	int missing = 0;

	for(int i = 0; i < game->playing->nearby_player_count; i++) {
		uint32_t id = game->playing->nearby_players[i].player_id;

		if(name_cache_is_known(id))
			continue;
		name_cache_lookup(id);
		missing++;
	}
	name_cache_flush_requests();
	return missing ? LOAD_STEP_WAIT : LOAD_STEP_DONE;
}

void join_loader_add_steps(load_queue* q, GameState* game) {
	load_queue_add(q, "Receiving your character", 2, 1, WAIT_REQUIRED, step_character, game);
	load_queue_add(q, "Checking the time", 1, 1, WAIT_REQUIRED, step_time, game);
	load_queue_add(q, "Receiving your stats", 1, 1, WAIT_REQUIRED, step_stats, game);
	load_queue_add(q, "Receiving your abilities", 1, 1, WAIT_REQUIRED, step_abilities, game);
	load_queue_add(q, "Receiving your quests", 1, 1, WAIT_REQUIRED, step_quests, game);
	load_queue_add(q, "Receiving your friends", 1, 1, WAIT_REQUIRED, step_friends, game);
	load_queue_add(q, "Receiving online players", 1, 1, WAIT_REQUIRED, step_online_list, game);
	load_queue_add(q, "Looking around", 1, 1, WAIT_REQUIRED, step_nearby, game);
	load_queue_add(q, "Learning names", 1, 1, WAIT_REQUIRED, step_names, game);
}
