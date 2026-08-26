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

#include <stdlib.h>

/**
 * Allocate gameplay state and initialize gameplay-only resources.
 *
 * The allocated PlayingState and textures are released by playing_exit.
 */
void playing_enter(GameState* game) {
    printf("[STATE] Entering gameplay\n");

    game->playing = calloc(1, sizeof(PlayingState));
    if (!game->playing) {
        fprintf(stderr, "[STATE] FATAL: Failed to allocate PlayingState\n");
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
    game->textures.player            = texture_load("Game/Sprites/Player/player.png");
    game->textures.session_panel_bg  = texture_load("Game/Sprites/UI/session_panel_bg.png");
    game->textures.session_entry_bg  = texture_load("Game/Sprites/UI/session_entry_bg.png");

    if (!game->textures.player)
        fprintf(stderr, "[GAME] Warning: failed to load player texture\n");

    ability_bar_init(&game->playing->ability_bar,
                     game->camera.viewport_width,
                     game->camera.viewport_height);

    hud_init(&game->playing->hud, 1920, 1080);
    quest_log_init(&game->playing->quest_log);
    currency_panel_init(&game->playing->currency_panel);

    /* No dialogue data to load: the server sends the page's text with the page,
     * so there is nothing here that could drift from what the NPC actually says. */
    dialogue_ui_init();

    extern GameState* g_current_game;
    g_current_game = game;
}

/**
 * Disconnect gameplay and release gameplay-only resources.
 */
void playing_exit(GameState* game) {
    printf("[STATE] Exiting gameplay\n");

    if (game->network_connected) {
        network_disconnect();
        game->network_connected = 0;
    }

    // Unload gameplay-only textures
    if (game->textures.player)           { texture_unload(game->textures.player);           game->textures.player           = 0; }
    if (game->textures.session_panel_bg) { texture_unload(game->textures.session_panel_bg); game->textures.session_panel_bg = 0; }
    if (game->textures.session_entry_bg) { texture_unload(game->textures.session_entry_bg); game->textures.session_entry_bg = 0; }
    // World tileset textures are unloaded by world_cleanup

    // Unload ability icon textures, then free the playing state
    if (game->playing) {
        ability_bar_cleanup(&game->playing->ability_bar);
        free(game->playing);
        game->playing = NULL;
    }
}

/** State-handler table for GAME_MODE_PLAYING. */
const StateHandler g_state_playing = {
    .enter = playing_enter,
    .exit = playing_exit,
    .update = playing_update,
    .render = playing_render,
    .handle_input = playing_input
};
