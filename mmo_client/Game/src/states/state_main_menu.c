/**
 * @file
 * Implement the client's initial menu state and its navigation choices.
 */

#include "state_handler.h"
#include "game.h"
#include "renderer.h"
#include "input/input.h"
#include <stdio.h>

static void main_menu_enter(GameState* game) {
    printf("[STATE] Entering main menu\n");
    game->main_menu.selected_button = -1;
    game->main_menu.hovered_button = -1;
}

static void main_menu_exit(GameState* game) {
    (void)game;
    printf("[STATE] Exiting main menu\n");
}

static void main_menu_update(GameState* game, float delta_time) {
    game->main_menu.animation_time += delta_time;
}

/**
 * Render the main menu and process pointer activation of its buttons.
 */
static void main_menu_render(GameState* game) {
    int vw = game->camera.viewport_width;
    int vh = game->camera.viewport_height;
    
    // Set up screen-space projection
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, vw, vh, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    
    // Draw background
    if (game->textures.background != 0) {
        renderer_draw_sprite(0, 0, vw, vh, game->textures.background);
    } else {
        renderer_draw_rect(0, 0, vw, vh, 0.1f, 0.1f, 0.2f, 1.0f);
    }
    
    // Menu panel
    float panel_w = 300;
    float panel_h = 350;
    float panel_x = (vw - panel_w) / 2;
    float panel_y = (vh - panel_h) / 2;
    
    renderer_draw_rect(panel_x, panel_y, panel_w, panel_h, 0.15f, 0.15f, 0.2f, 0.9f);
    
    // Title area
    renderer_draw_rect(panel_x, panel_y, panel_w, 60, 0.2f, 0.3f, 0.4f, 1.0f);
    renderer_draw_text(panel_x + 70, panel_y + 40, "MULTIVERSE");
    
    // Buttons
    const char* buttons[] = {"Play", "Settings", "Quit"};
    int button_count = 3;
    float btn_w = 200;
    float btn_h = 50;
    float btn_x = panel_x + (panel_w - btn_w) / 2;
    float btn_start_y = panel_y + 100;
    float btn_spacing = 70;
    
    for (int i = 0; i < button_count; i++) {
        float btn_y = btn_start_y + i * btn_spacing;
        
        int hovered = input_mouse_in_rect(&game->input, btn_x, btn_y, btn_w, btn_h);
        game->main_menu.hovered_button = hovered ? i : game->main_menu.hovered_button;
        
        float color = hovered ? 0.4f : 0.25f;
        renderer_draw_rect(btn_x, btn_y, btn_w, btn_h, color, color, color + 0.1f, 1.0f);
        renderer_draw_text(btn_x + 70, btn_y + 35, buttons[i]);
        
        // Handle click
        if (hovered && game->input.mouse_left_clicked) {
            switch (i) {
                case 0: // Play
                    game_change_state(game, GAME_MODE_SERVER_LIST);
                    break;
                case 1: // Settings
                    game_change_state(game, GAME_MODE_SETTINGS);
                    break;
                case 2: // Quit
                    game->is_running = 0;
                    break;
            }
        }
    }
    
    // Network status
    float status_x = vw - 260;
    float status_y = vh - 40;
    renderer_draw_rect(status_x, status_y, 250, 30, 0.1f, 0.1f, 0.15f, 0.8f);
    
    if (game->network_connected) {
        renderer_draw_rect(status_x + 5, status_y + 5, 20, 20, 0.2f, 0.8f, 0.2f, 1.0f);
    } else {
        renderer_draw_rect(status_x + 5, status_y + 5, 20, 20, 0.8f, 0.2f, 0.2f, 1.0f);
    }
}

static void main_menu_input(GameState* game, GLFWwindow* window, float delta_time) {
    (void)delta_time;
    
    if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) {
        game->is_running = 0;
    }
}

/** State-handler table for GAME_MODE_MAIN_MENU. */
const StateHandler g_state_main_menu = {
    .enter = main_menu_enter,
    .exit = main_menu_exit,
    .update = main_menu_update,
    .render = main_menu_render,
    .handle_input = main_menu_input
};
