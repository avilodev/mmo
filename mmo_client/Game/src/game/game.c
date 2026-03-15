#include "game.h"
#include "renderer.h"
#include "texture.h"
#include "camera.h"
#include "input.h"
#include "world.h"
#include "ability_bar.h"
#include "player.h"
#include "state_handler.h"
#include "combat_system.h"
#include "inventory.h"
#include "character_screen.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void game_init(GameState* game, int viewport_width, int viewport_height) {
    memset(game, 0, sizeof(GameState));
    
    game->is_running = 1;
    game->mode = GAME_MODE_MAIN_MENU;
    game->net_state = NET_STATE_IDLE;
    
    // Initialize subsystems
    input_init(&game->input);
    player_init(&game->player);
    camera_init(&game->camera, viewport_width, viewport_height);
    combat_init(&game->combat);
    ability_bar_init(&game->ability_bar, viewport_width, viewport_height);
    
    // Initialize item database (once at startup)
    item_db_init();
    
    // Allocate and initialize inventory
    game->inventory = malloc(sizeof(InventoryState));
    if (game->inventory) {
        inventory_init(game->inventory, viewport_width, viewport_height);
    } else {
        fprintf(stderr, "[GAME] Failed to allocate inventory!\n");
    }
    
    // Allocate and initialize character screen
    game->character_screen = malloc(sizeof(CharacterScreenState));
    if (game->character_screen) {
        character_screen_init(game->character_screen, viewport_width, viewport_height);
    } else {
        fprintf(stderr, "[GAME] Failed to allocate character screen!\n");
    }
    
    // Initialize world with CHUNKED LOADING from binary file
    if (!world_init(&game->world, "Game/bin/world.dat", 16)) {
        fprintf(stderr, "[GAME] Failed to initialize world!\n");
        fprintf(stderr, "[GAME] Make sure world.dat exists in the game directory!\n");
        fprintf(stderr, "[GAME] Run world_generator to create it.\n");
    }
    
    // Set initial player position (center of walkable area - top third)
    // World is 256x256 tiles, so place player at (128, 64) in tiles
    game->player.x = 128 * 16 + 8;  // Center horizontally, tile_size=16
    game->player.y = 64 * 16 + 8;   // In the grass area (top third)
    
    camera_set_position(&game->camera, game->player.x, game->player.y);
    
    // Load textures
    game->textures.player = texture_load("Game/Sprites/Player/player.png");
    game->textures.grass = texture_load("Game/Sprites/World/grass.png");
    game->textures.water = texture_load("Game/Sprites/World/water.png");
    game->textures.rock = texture_load("Game/Sprites/World/rock.png");
    game->textures.background = texture_load("Game/Sprites/Background/background.png");
    game->textures.tree1 = texture_load("Game/Sprites/Decoration/Tree/tree1.png");
    game->textures.shrub1 = texture_load("Game/Sprites/Decoration/Bush/bush1.png");
    
    if (game->textures.player == 0) {
        fprintf(stderr, "[GAME] Warning: Failed to load player texture\n");
    }
    
    game->background_width = 860;
    game->background_height = 458;
    
    // Initialize menu states
    game->main_menu.selected_button = -1;
    game->server_list.selected_index = -1;
    game->char_select.selected_index = -1;
    game->char_select.selected_class = 1;
    game->char_select.selected_race = 1;
    
    // Enter initial state
    const StateHandler* handler = state_handler_get(game->mode);
    if (handler && handler->enter) {
        handler->enter(game);
    }
    
    printf("[GAME] Initialized\n");
    printf("[GAME] World: %dx%d tiles (chunked loading)\n", 
           game->world.world_width, game->world.world_height);
    printf("[GAME] Viewport: %dx%d\n", viewport_width, viewport_height);
}

void game_change_state(GameState* game, GameMode new_mode) {
    if (game->mode == new_mode) {
        printf("[GAME] Already in mode %d, skipping\n", new_mode);
        return;
    }
    
    printf("[GAME] Changing state: %d -> %d\n", game->mode, new_mode);
    
    // Exit current state
    const StateHandler* old_handler = state_handler_get(game->mode);
    if (old_handler && old_handler->exit) {
        printf("[GAME] Calling exit for mode %d\n", game->mode);
        old_handler->exit(game);
    }
    
    game->mode = new_mode;
    
    // Enter new state
    const StateHandler* new_handler = state_handler_get(new_mode);
    if (new_handler && new_handler->enter) {
        printf("[GAME] Calling enter for mode %d\n", new_mode);
        new_handler->enter(game);
    }
}

void game_handle_input(GameState* game, GLFWwindow* window, float delta_time) {
    // Get window size for mouse scaling
    int win_w, win_h;
    glfwGetWindowSize(window, &win_w, &win_h);
    
    // Update input state
    input_update(&game->input, window, 
                game->camera.viewport_width, 
                game->camera.viewport_height,
                win_w, win_h);
    
    // Delegate to current state handler
    const StateHandler* handler = state_handler_get(game->mode);
    if (handler && handler->handle_input) {
        handler->handle_input(game, window, delta_time);
    }
}

void game_update(GameState* game, float delta_time) {
    // Update world chunks based on player position (critical for chunked loading!)
    world_update_chunks(&game->world, game->player.x, game->player.y);
    
    // Update temporary tile modifications (ability walls, etc.)
    world_update_modifications(&game->world, delta_time);
    
    // Delegate to current state handler
    const StateHandler* handler = state_handler_get(game->mode);
    if (handler && handler->update) {
        handler->update(game, delta_time);
    }
}

void game_render(GameState* game) {
    renderer_clear(0.1f, 0.1f, 0.2f);
    
    // Delegate to current state handler
    const StateHandler* handler = state_handler_get(game->mode);
    if (handler && handler->render) {
        handler->render(game);
    }
}

void game_cleanup(GameState* game) {
    // Exit current state
    const StateHandler* handler = state_handler_get(game->mode);
    if (handler && handler->exit) {
        handler->exit(game);
    }
    
    // Free character screen
    if (game->character_screen) {
        free(game->character_screen);
        game->character_screen = NULL;
    }
    
    // Free inventory
    if (game->inventory) {
        free(game->inventory);
        game->inventory = NULL;
    }
    
    // Free world (closes file, frees chunks)
    world_cleanup(&game->world);
    
    // Unload textures
    if (game->textures.player) texture_unload(game->textures.player);
    if (game->textures.grass) texture_unload(game->textures.grass);
    if (game->textures.water) texture_unload(game->textures.water);
    if (game->textures.rock) texture_unload(game->textures.rock);
    if (game->textures.background) texture_unload(game->textures.background);

    if (game->textures.tree1) texture_unload(game->textures.tree1);
    if (game->textures.shrub1) texture_unload(game->textures.shrub1);
    
    printf("[GAME] Cleaned up\n");
}