/**
 * @file
 * Enter and leave the gameplay state, and name its five entry points.
 *
 * This file used to be the whole of playing -- 1,835 lines covering the
 * simulation step, the world draw, ten overlays and the input router. What is
 * left here is the part that is genuinely about the state itself: what is
 * allocated on the way in, what is released on the way out, and the table that
 * tells the state machine where the rest of it lives.
 *
 * See states/state_playing_internal.h for how the rest was divided.
 */
#include "states/state_playing_internal.h"
#include "texture/texture.h"
#include "state_handler.h"
#include "network.h"
#include "combat_system.h"
#include "hud.h"
#include "ui/npc_dialogue.h"
#include "states/world_loader.h"
#include "states/session_reset.h"
#include "ui/weather_icon.h"
#include "audio/weather_sound.h"

#include <stdlib.h>
#include "core/client_log.h"

// The files of the info bar's ping icons, the worst level first
static const char* const HUD_PING_ICONS[NET_PING_LEVELS] = {
	"Game/Sprites/UI/Hud/ping_1_red.png",
	"Game/Sprites/UI/Hud/ping_2_orange.png",
	"Game/Sprites/UI/Hud/ping_3_yellow.png",
	"Game/Sprites/UI/Hud/ping_4_green.png",
};

static void load_hud_icons(GameTextures* textures) {
	// A condition without art keeps texture 0 and draws as a placeholder
	for(int i = 0; i < WEATHER_CONDITION_COUNT; i++)
		textures->weather_icons[i] = texture_load_icon(weather_icon_path(i));
	textures->weather_clear_night = texture_load_icon(weather_icon_night_path());

	for(int i = 0; i < NET_PING_LEVELS; i++)
		textures->hud_ping[i] = texture_load_icon(HUD_PING_ICONS[i]);
}

static void unload_texture(unsigned int* texture) {
	if(!*texture)
		return;

	texture_unload(*texture);
	*texture = 0;
}

static void unload_hud_icons(GameTextures* textures) {
	for(int i = 0; i < WEATHER_CONDITION_COUNT; i++)
		unload_texture(&textures->weather_icons[i]);
	unload_texture(&textures->weather_clear_night);

	for(int i = 0; i < NET_PING_LEVELS; i++)
		unload_texture(&textures->hud_ping[i]);
}

/**
 * Allocate gameplay state and initialize gameplay-only resources.
 *
 * The allocated PlayingState and textures are released by playing_exit.
 */
void playing_enter(GameState* game) {
    CLOG_INFO("[STATE] Entering gameplay");

    game->playing = calloc(1, sizeof(PlayingState));
    if (!game->playing) {
        CLOG_ERROR("[STATE] FATAL: Failed to allocate PlayingState");
        game->is_running = 0;
        return;
    }
    game->playing->map_zoom = 1.0f;

    /* move_sync starts zeroed by the calloc, which is what "nothing has been
     * sent yet" means -- it used to need an explicit reset here because it was
     * a static that outlived the session.
     *
     * hovered_effect does need setting: zero is a valid buff index, and -1 is
     * the "nothing under the cursor" value. */
    game->playing->hovered_effect = -1;

    combat_init(&game->playing->combat);

    // Load gameplay-only textures
    game->textures.session_panel_bg  = texture_load("Game/Sprites/UI/session_panel_bg.png");
    game->textures.session_entry_bg  = texture_load("Game/Sprites/UI/session_entry_bg.png");
	load_hud_icons(&game->textures);

    ability_bar_init(&game->playing->ability_bar,
                     game->camera.viewport_width,
                     game->camera.viewport_height);

    hud_init(&game->playing->hud, 1920, 1080);
    quest_log_init(&game->playing->quest_log);
    currency_panel_init(&game->playing->currency_panel);
    friends_panel_init(&game->playing->friends);

    /* No dialogue data to load: the server sends the page's text with the page,
     * so there is nothing here that could drift from what the NPC actually says. */
    dialogue_ui_init();

    extern GameState* g_current_game;
    g_current_game = game;

	// The world files load, and the server's join data is awaited, behind a loading
	// screen. Begun first: it forgets what arrived, and the replay below is part of it.
	world_loader_begin(game);

	// The server's join packets came in during character select; deliver them now.
	network_replay_early_quest_packets();
}

/**
 * Disconnect gameplay and release gameplay-only resources.
 */
void playing_exit(GameState* game) {
    CLOG_INFO("[STATE] Exiting gameplay");

    if (game->network_connected) {
        network_disconnect();
        game->network_connected = 0;
    }

	// The weather's sound goes with the world it was in
	weather_sound_silence();
	// The world and city assets go with the session
	world_loader_unload(game);

    // Unload gameplay-only textures
    if (game->textures.session_panel_bg) { texture_unload(game->textures.session_panel_bg); game->textures.session_panel_bg = 0; }
    if (game->textures.session_entry_bg) { texture_unload(game->textures.session_entry_bg); game->textures.session_entry_bg = 0; }
	unload_hud_icons(&game->textures);
    // World tileset textures are unloaded by world_cleanup

    // Unload ability icon textures, then free the playing state
    if (game->playing) {
        ability_bar_cleanup(&game->playing->ability_bar);
        free(game->playing);
        game->playing = NULL;
    }

	// Everything else the session left: open windows, the camera, the body, caches
	session_reset(game);
}

/** State-handler table for GAME_MODE_PLAYING. */
const StateHandler g_state_playing = {
    .enter = playing_enter,
    .exit = playing_exit,
    .update = playing_update,
    .render = playing_render,
    .handle_input = playing_input
};
