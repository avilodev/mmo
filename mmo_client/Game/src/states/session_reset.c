// Reset what survives a session outside the playing state's own allocation.

#include "states/session_reset.h"
#include "camera/camera.h"
#include "network/name_cache.h"
#include "ui/emote_hub.h"
#include "ui/weather_window.h"
#include "player/move_refusal.h"
#include "player.h"
#include "inventory.h"
#include "character_screen.h"
#include "render/character_renderer.h"
#include "ui/map_view.h"
#include "ui/settings_menu.h"
#include "world_regions.h"
#include "world/world_clock.h"
#include "world/weather_state.h"
#include "world/weather_now.h"

void session_reset(GameState* game) {
	float view_w = (float)game->camera.viewport_width;
	float view_h = (float)game->camera.viewport_height;
	const WorldCity* start = world_city_find(CITY_ENNARA);

	// Debug and caches that live in files of their own
	emote_hub_reset();
	move_refusal_reset();
	name_cache_reset();
	character_renderer_reset();
	map_view_shutdown();
	world_clock_reset();
	weather_state_reset();
	// the ground is made again from the world's weather on the next login, and a
	// weather forced for a look does not outlast the session
	weather_now_reset();
	weather_now_force_day(NULL, 0);
	weather_window_reset();

	// Windows and menus left open
	sm_close();
	game->show_settings = 0;
	if(game->inventory)
		inventory_init(game->inventory, view_w, view_h);
	if(game->character_screen)
		character_screen_init(game->character_screen, view_w, view_h);
	if(game->inspect_screen)
		character_screen_init(game->inspect_screen, view_w, view_h);

	// The body and the view of it, as the game starts them
	player_init(&game->player);
	camera_init(&game->camera, game->camera.viewport_width, game->camera.viewport_height);
	world_city_center_px(start, &game->player.x, &game->player.y);
	camera_set_position(&game->camera, game->player.x, game->player.y);
}
