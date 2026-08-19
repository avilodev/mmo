/**
 * @file
 * Coordinate client game state, mode handlers, persistent settings, and subsystem lifetimes.
 */

#include "game.h"
#include "renderer.h"
#include "texture.h"
#include "camera.h"
#include "input.h"
#include "world.h"
#include "npc_types.h"
#include "ability_bar.h"
#include "player.h"
#include "state_handler.h"
#include "combat_system.h"
#include "inventory.h"
#include "character_screen.h"
#include "audio/audio.h"
#include "core/keybinds.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <direct.h>   // _mkdir

/**
 * Initialize the game state and its shared client subsystems.
 *
 * The inventory and character screen are heap-allocated and released by game_cleanup.
 *
 * @param game  Game state storage to initialize.
 * @param viewport_width  Logical viewport width in pixels.
 * @param viewport_height  Logical viewport height in pixels.
 */
void game_init(GameState* game, int viewport_width, int viewport_height) {
    memset(game, 0, sizeof(GameState));
    
    game->is_running = 1;
    game->mode = GAME_MODE_MAIN_MENU;
    game->net_state = NET_STATE_IDLE;

    // Default settings
    game->settings.master_volume = 0.7f;
    game->settings.music_volume  = 0.5f;
    game->settings.sfx_volume    = 0.8f;
    game->settings.show_fps      = 0;
    game->settings.fullscreen    = 0;
    game->settings.ui_scale      = 1.0f;
    
    // Initialize subsystems
    input_init(&game->input);
    player_init(&game->player);
    camera_init(&game->camera, viewport_width, viewport_height);
    game->playing = NULL;
    
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
    
    // Ensure data directory exists before any file reads/writes
    _mkdir("Game/data");

    // Load NPC type name table
    npc_types_init("Game/data/npc_types.json");

    // Load saved settings (overrides defaults if file exists)
    game_settings_load(&game->settings, SETTINGS_PATH);

    // Load keybinds (overrides defaults if file exists)
    keybinds_load("Game/data/keybinds.cfg");

    // Initialize audio and apply loaded settings
    audio_init();
    game_settings_apply(&game->settings);

    // Initialize world with CHUNKED LOADING from binary file
    if (!world_init(&game->world, "Game/bin/world.dat", 16)) {
        fprintf(stderr, "[GAME] FATAL: Failed to initialize world!\n");
        fprintf(stderr, "[GAME] Make sure world.dat exists in the game directory!\n");
        fprintf(stderr, "[GAME] Run world_generator to create it.\n");
        game->is_running = 0;
        return;
    }
    
    // Ennara Courtyard centre — matches worldgen_spawn_point() and the
    // server-side default in player_data.c.
    game->player.x = 222400.0f;
    game->player.y = 89280.0f;
    
    camera_set_position(&game->camera, game->player.x, game->player.y);
    
    // Load only menu/shared assets at startup.
    // Gameplay textures (player, tiles, decorations) are loaded in playing_enter
    // and unloaded in playing_exit.
    game->textures.background = texture_load("Game/Sprites/Background/background.png");
    
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

/**
 * Exit the current mode and enter a new game mode.
 *
 * @param game  Initialized game state whose active handler is changed.
 */
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

    // Consume the click that triggered this transition so the new state's
    // first update frame doesn't see it and accidentally fire a button.
    game->input.mouse_left_clicked  = 0;
    game->input.mouse_right_clicked = 0;

    // Enter new state
    const StateHandler* new_handler = state_handler_get(new_mode);
    if (new_handler && new_handler->enter) {
        printf("[GAME] Calling enter for mode %d\n", new_mode);
        new_handler->enter(game);
    }
}

/**
 * Refresh input state and dispatch input to the active mode handler.
 *
 * @param window  Active GLFW window used for input and coordinate scaling.
 * @param delta_time  Elapsed frame time in seconds.
 */
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

/**
 * Update streamed world data and the active game mode.
 *
 * @param delta_time  Elapsed frame time in seconds.
 */
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

/**
 * Clear the frame and render the active game mode.
 */
void game_render(GameState* game) {
    renderer_clear(0.1f, 0.1f, 0.2f);
    
    // Delegate to current state handler
    const StateHandler* handler = state_handler_get(game->mode);
    if (handler && handler->render) {
        handler->render(game);
    }
}

/**
 * Release game-owned resources and persist current settings.
 */
void game_cleanup(GameState* game) {
    printf("[SHUTDOWN]   state exit...\n"); fflush(stdout);
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
    
    // Free NPC type table
    npc_types_cleanup();

    printf("[SHUTDOWN]   world...\n"); fflush(stdout);
    // Free world (closes file, frees chunks)
    world_cleanup(&game->world);
    
    // Unload menu/shared textures
    // (gameplay textures are already unloaded by playing_exit)
    if (game->textures.background) texture_unload(game->textures.background);
    
    printf("[SHUTDOWN]   settings + audio...\n"); fflush(stdout);
    // Save settings and shut down audio
    game_settings_save(&game->settings, SETTINGS_PATH);
    audio_cleanup();

    printf("[GAME] Cleaned up\n");
    fflush(stdout);
}

/**
 * Write client settings to a text configuration file.
 *
 * @param s  Settings values to serialize.
 * @param path  Destination file path, which is replaced when writable.
 */
void game_settings_save(const GameSettings* s, const char* path) {
    FILE* f = fopen(path, "w");
    if (!f) {
        printf("[GAME] Warning: could not save settings to %s\n", path);
        return;
    }
    fprintf(f, "master_volume=%.4f\n", s->master_volume);
    fprintf(f, "music_volume=%.4f\n",  s->music_volume);
    fprintf(f, "sfx_volume=%.4f\n",    s->sfx_volume);
    fprintf(f, "show_fps=%d\n",        s->show_fps);
    fprintf(f, "fullscreen=%d\n",      s->fullscreen);
    fprintf(f, "ui_scale=%.4f\n",      s->ui_scale);
    fclose(f);
    printf("[GAME] Settings saved to %s\n", path);
}

/**
 * Load recognized client settings from a text configuration file.
 *
 * Missing files and unrecognized lines leave the corresponding values unchanged.
 *
 * @param s  Existing settings to update with parsed values.
 * @param path  Configuration file path to read.
 */
void game_settings_load(GameSettings* s, const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) return;  // No file yet — keep defaults
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        char key[64]; float fval; int ival;
        if (sscanf(line, "master_volume=%f", &fval) == 1) {
            s->master_volume = (fval < 0.0f) ? 0.0f : (fval > 1.0f) ? 1.0f : fval;
        } else if (sscanf(line, "music_volume=%f", &fval) == 1) {
            s->music_volume  = (fval < 0.0f) ? 0.0f : (fval > 1.0f) ? 1.0f : fval;
        } else if (sscanf(line, "sfx_volume=%f", &fval) == 1) {
            s->sfx_volume    = (fval < 0.0f) ? 0.0f : (fval > 1.0f) ? 1.0f : fval;
        } else if (sscanf(line, "show_fps=%d", &ival) == 1) {
            s->show_fps = ival ? 1 : 0;
        } else if (sscanf(line, "fullscreen=%d", &ival) == 1) {
            s->fullscreen = ival ? 1 : 0;
        } else if (sscanf(line, "ui_scale=%f", &fval) == 1) {
            s->ui_scale = (fval < 0.75f) ? 0.75f : (fval > 1.5f) ? 1.5f : fval;
        }
        (void)key;
    }
    fclose(f);
    printf("[GAME] Settings loaded from %s\n", path);
}

// Global window handle — defined here, declared extern in game.h
GLFWwindow* g_window = NULL;

// Saved windowed-mode geometry for restoring after fullscreen exit
static int s_win_x = 100, s_win_y = 100, s_win_w = 1728, s_win_h = 972;

/**
 * Apply audio levels and fullscreen state to active client subsystems.
 *
 * The global window pointer may be NULL before window creation or after shutdown.
 *
 * @param s  Settings values to apply.
 */
void game_settings_apply(const GameSettings* s) {
    audio_set_master_volume(s->master_volume);
    audio_set_music_volume(s->music_volume);
    audio_set_sfx_volume(s->sfx_volume);

    if (g_window) {
        static int prev_fullscreen = -1;
        if (s->fullscreen != prev_fullscreen) {
            prev_fullscreen = s->fullscreen;
            if (s->fullscreen) {
                // Save current windowed geometry before going fullscreen
                glfwGetWindowPos(g_window,  &s_win_x, &s_win_y);
                glfwGetWindowSize(g_window, &s_win_w, &s_win_h);
                GLFWmonitor* mon = glfwGetPrimaryMonitor();
                const GLFWvidmode* vm = glfwGetVideoMode(mon);
                glfwSetWindowMonitor(g_window, mon, 0, 0,
                                     vm->width, vm->height, vm->refreshRate);
            } else {
                glfwSetWindowMonitor(g_window, NULL,
                                     s_win_x, s_win_y, s_win_w, s_win_h, 0);
            }
        }
    }
}
